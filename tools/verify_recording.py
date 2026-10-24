"""验证真实记录/传感器代码的故障路径，以及新旧 USB 导出协议。"""
import csv
import io
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import export_recording as exporter

ROOT = Path(__file__).resolve().parents[1]

class SerialChunks:
    def __init__(self, chunks):
        self.chunks = list(chunks)
        self.read_sizes = []
        self.closed = False

    @property
    def in_waiting(self):
        return len(self.chunks[0]) if self.chunks else 0

    def read(self, size):
        self.read_sizes.append(size)
        if not self.chunks:
            return b''
        chunk = self.chunks[0][:size]
        self.chunks[0] = self.chunks[0][size:]
        if not self.chunks[0]:
            self.chunks.pop(0)
        return chunk

    def __enter__(self):
        return self

    def __exit__(self, *arguments):
        self.closed = True

    def open(self):
        assert self.dtr is False and self.rts is False

    def reset_input_buffer(self):
        pass

    def write(self, data):
        assert data == b'e'


class ExportTests(unittest.TestCase):
    def stream(self, version=3):
        header = dict(event='recording_session', format=version, device_id='device', session_id='0000000001',
                      source='classroom_measurement', sample_origin='', model_sha256='model', started_uptime_ms=0,
                      clock_source='none', timezone_offset_minutes=0, clock_set_epoch_ms=0, clock_set_uptime_ms=0)
        row = '1,30000,0,,,0,nan,nan,nan,0,stale,2,nan,nan,sensor_fault,stale' if version == 3 else '1,5000,0,,,0,900,25,60,1,none,2,nan,nan'
        if version == 4:
            row = '1,1000,0,,0,5,15,900,850,900,850,800,700,0,model,fan_assumption=nominal'
            return [json.dumps(dict(event='recording_export_begin', format=4)), json.dumps(header),
                    'recording_forecast,' + row, json.dumps(dict(event='recording_export_end', records=1, sessions=1, incomplete_rows=0))]
        return [json.dumps(dict(event='recording_export_begin', format=version)), json.dumps(header),
                'recording_row,' + row, json.dumps(dict(event='recording_export_end', records=1, sessions=1, incomplete_rows=0))]

    def test_legacy_and_fault_retained(self):
        for version in (1, 2, 3):
            target = io.StringIO()
            result = exporter.read_export(self.stream(version), csv.DictWriter(target, exporter.CSV_FIELDS))
            self.assertEqual(result['records'], 1)
            row = next(csv.DictReader(io.StringIO(target.getvalue())))
            self.assertEqual(row['record_type'], 'sensor_fault' if version == 3 else 'legacy_observation')
            self.assertEqual(row['valid'], '0' if version == 3 else '1')

    def test_truncation_rejected(self):
        with self.assertRaises(ValueError):
            exporter.read_export(self.stream()[:-1], csv.DictWriter(io.StringIO(), exporter.CSV_FIELDS))

    def test_serial_chunks_preserve_utf8_crlf_and_multiple_lines(self):
        connection = SerialChunks([b'recording_row,1,\xe4', b'\xb8\xad\r', b'\n\nrecording_forecast,2\ntrailing'])
        raw = io.BytesIO()
        lines = exporter.serial_lines(connection, 20, raw)
        self.assertEqual(next(lines), 'recording_row,1,中')
        self.assertEqual(next(lines), 'recording_forecast,2')
        self.assertEqual(raw.getvalue(), b'recording_row,1,\xe4\xb8\xad\r\n\nrecording_forecast,2\ntrailing')
        self.assertGreater(max(connection.read_sizes), 1)

    def test_serial_idle_timeout_and_partial_line_rejected(self):
        for chunks, error in (([], TimeoutError), ([b'recording_row,1'], ValueError)):
            with self.subTest(chunks=chunks), patch.object(exporter.time, 'monotonic', side_effect=[0, 0, 2]):
                with self.assertRaises(error):
                    list(exporter.serial_lines(SerialChunks(chunks), 1))

    def test_serial_line_limit_allows_many_lines_but_rejects_long_line(self):
        lines = exporter.serial_lines(SerialChunks([b'ok\n' * 2000]), 20)
        self.assertEqual([next(lines) for _ in range(2000)], ['ok'] * 2000)
        oversized = b'x' * (exporter.MAX_SERIAL_LINE_BYTES + 1)
        for chunks in ([oversized + b'\n'], [oversized[:2000], oversized[2000:]]):
            with self.subTest(terminated=chunks[-1].endswith(b'\n')):
                with self.assertRaisesRegex(ValueError, '过长'):
                    next(exporter.serial_lines(SerialChunks(chunks), 20))

    def test_serial_only_protocol_lines_extend_idle_timeout(self):
        connection = SerialChunks([b'debug\n', b'recording_row,1\n', b'debug\n'])
        with patch.object(exporter.time, 'monotonic', side_effect=[0, 0, 0.5, 0.9, 1.1, 2]):
            lines = exporter.serial_lines(connection, 1)
            self.assertEqual([next(lines), next(lines), next(lines)], ['debug', 'recording_row,1', 'debug'])
            with self.assertRaises(TimeoutError):
                next(lines)

    def test_export_spools_before_validation_and_keeps_file_protection(self):
        payload = ('\r\n'.join(self.stream()) + '\r\n').encode()
        connection = SerialChunks([payload])
        original_parser = exporter.read_export
        def parse_after_close(*arguments):
            self.assertTrue(connection.closed)
            return original_parser(*arguments)
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'recording.csv'
            with patch.object(exporter.serial, 'Serial', return_value=connection), patch.object(exporter, 'read_export', side_effect=parse_after_close):
                result = exporter.export('mock', output, None, 20)
            self.assertEqual(result['records'], 1)
            self.assertEqual(result['raw_path'].read_bytes(), payload)
            self.assertFalse(output.with_suffix('.csv.partial').exists())
            result['raw_path'].unlink()
            with patch.object(exporter.serial, 'Serial') as serial_factory:
                with self.assertRaises(FileExistsError):
                    exporter.export('mock', output, None, 20)
                output.unlink()
                partial = output.with_suffix('.csv.partial')
                partial.write_bytes(b'preserve')
                with self.assertRaises(FileExistsError):
                    exporter.export('mock', output, None, 20)
                self.assertEqual(partial.read_bytes(), b'preserve')
                serial_factory.assert_not_called()

    def test_export_timeout_preserves_raw_and_partial(self):
        payload = ('\n'.join(self.stream()[:-1]) + '\n').encode()
        connection = SerialChunks([payload])
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'recording.csv'
            with patch.object(exporter.serial, 'Serial', return_value=connection), patch.object(exporter.time, 'monotonic', side_effect=[0, 0, 0, 2]), patch.object(exporter.sys, 'stderr', new_callable=io.StringIO) as errors:
                with self.assertRaises(TimeoutError):
                    exporter.export('mock', output, None, 1)
            raw_path = Path(errors.getvalue().strip().removeprefix('原始 USB 数据：'))
            self.assertEqual(raw_path.read_bytes(), payload)
            self.assertFalse(output.exists())
            with output.with_suffix('.csv.partial').open(encoding='utf-8-sig') as handle:
                rows = list(csv.DictReader(handle))
            self.assertEqual(len(rows), 1)
            raw_path.unlink()

    def test_board_error_reports_cause_before_timeout_and_preserves_evidence(self):
        for code in ('usb_export_timeout', 'record_unreadable'):
            for partial_line in ('', '{"event":"recording_sess'):
                before_error = ('\n'.join(self.stream()[:-1]) + '\n' + partial_line + '\n').encode()
                error_line = (json.dumps(dict(event='recording_error', code=code)) + '\n').encode()
                payload = before_error + error_line
                connection = SerialChunks([before_error, error_line])
                with self.subTest(code=code, partial=bool(partial_line)), tempfile.TemporaryDirectory() as directory:
                    output = Path(directory) / 'recording.csv'
                    with patch.object(exporter.serial, 'Serial', return_value=connection), patch.object(exporter.time, 'monotonic', side_effect=[0] * 12 + [30]), patch.object(exporter.sys, 'stderr', new_callable=io.StringIO) as errors:
                        with self.assertRaisesRegex(ValueError, exporter.ERROR_MESSAGES[code]):
                            exporter.export('mock', output, None, 20)
                    raw_path = Path(errors.getvalue().strip().removeprefix('原始 USB 数据：'))
                    self.assertEqual(raw_path.read_bytes(), payload)
                    self.assertTrue(connection.closed)
                    self.assertFalse(output.exists())
                    self.assertTrue(output.with_suffix('.csv.partial').exists())
                    raw_path.unlink()

    def test_wrong_sequence_rejected(self):
        lines = self.stream(); lines[2] = lines[2].replace('recording_row,1,', 'recording_row,2,')
        with self.assertRaises(ValueError):
            exporter.read_export(lines, csv.DictWriter(io.StringIO(), exporter.CSV_FIELDS))

    def test_forecast_snapshot_exported_separately(self):
        target = io.StringIO()
        result = exporter.read_export(self.stream(4), csv.DictWriter(target, exporter.CSV_FIELDS))
        self.assertEqual(result['records'], 1)
        row = next(csv.DictReader(io.StringIO(target.getvalue())))
        self.assertEqual(row['record_type'], 'prediction_snapshot')
        self.assertEqual(row['forecast_target_5_minute'], '5')
        self.assertEqual(row['forecast_target_15_minute'], '15')
        self.assertEqual(row['forecast_fan_15_ppm'], '700')

    def test_prediction_horizons_match_session_version(self):
        for version in (3, 4):
            lines = self.stream(version)
            lines[2] = 'recording_row,1,30000,0,,,0,900,25,60,1,none,0,920,950,measurement,'
            target = io.StringIO()
            exporter.read_export(lines, csv.DictWriter(target, exporter.CSV_FIELDS))
            row = next(csv.DictReader(io.StringIO(target.getvalue())))
            self.assertEqual(row['prediction_5_ppm'], '920' if version == 4 else '')
            self.assertEqual(row['prediction_15_ppm'], '950' if version == 4 else '920')
            self.assertEqual(row['prediction_30_ppm'], '' if version == 4 else '950')

    def test_unavailable_forecast_retained_and_success_nan_rejected(self):
        lines = self.stream(4)
        lines[2] = 'recording_forecast,1,1000,0,,0,5,15,nan,nan,nan,nan,nan,nan,2,model,history_gap'
        target = io.StringIO()
        exporter.read_export(lines, csv.DictWriter(target, exporter.CSV_FIELDS))
        row = next(csv.DictReader(io.StringIO(target.getvalue())))
        self.assertEqual(row['forecast_status'], '2')
        lines[2] = lines[2].replace(',2,model,', ',0,model,')
        with self.assertRaises(ValueError):
            exporter.read_export(lines, csv.DictWriter(io.StringIO(), exporter.CSV_FIELDS))

    def test_disabled_forecast_retains_status_and_empty_values(self):
        lines = self.stream(4)
        lines[2] = 'recording_forecast,1,1000,0,,0,5,15,nan,nan,nan,nan,nan,nan,5,model,disabled'
        target = io.StringIO()
        exporter.read_export(lines, csv.DictWriter(target, exporter.CSV_FIELDS))
        row = next(csv.DictReader(io.StringIO(target.getvalue())))
        self.assertEqual(row['forecast_status'], '5')
        self.assertEqual(row['forecast_5_ppm'], 'nan')
        lines[2] = 'recording_row,1,30000,0,,,0,900,25,60,1,none,5,nan,nan,measurement,'
        target = io.StringIO()
        exporter.read_export(lines, csv.DictWriter(target, exporter.CSV_FIELDS))
        self.assertEqual(next(csv.DictReader(io.StringIO(target.getvalue())))['prediction_status'], '5')
        lines[2] = lines[2].replace(',5,nan,nan', ',6,nan,nan')
        with self.assertRaises(ValueError):
            exporter.read_export(lines, csv.DictWriter(io.StringIO(), exporter.CSV_FIELDS))
        for version in (1, 2, 3):
            legacy = self.stream(version)
            legacy[2] = legacy[2].replace(',2,nan,nan', ',5,nan,nan')
            with self.assertRaises(ValueError):
                exporter.read_export(legacy, csv.DictWriter(io.StringIO(), exporter.CSV_FIELDS))

STUB = r'''
#pragma once
#include <cassert>
#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <map>
#include <vector>
#include <memory>
#include <cmath>
using std::isfinite;
#include <ctime>
inline tm* gmtime_r(const time_t* t, tm* out){return gmtime_s(out,t)==0?out:nullptr;}
inline uint64_t simulatedTime = 0;
inline uint32_t millis() { return simulatedTime; }
inline void delay(unsigned value) { simulatedTime += value; }
inline int64_t esp_timer_get_time() { return int64_t(simulatedTime) * 1000; }
inline int esp_reset_reason() { return 3; }
struct String : std::string { using std::string::string; String(std::string s):std::string(s){} };
struct SerialMock {
 std::string output; bool connected=true;
 size_t maximumWrite=2048, pauseAt=SIZE_MAX; std::string pauseOnText; uint64_t zeroWriteUntil=0; uint64_t pauseStarted=UINT64_MAX; unsigned pauseDuration=0;
 bool ready() {
  if((output.size()>=pauseAt || (!pauseOnText.empty() && output.find(pauseOnText)!=std::string::npos)) && pauseStarted==UINT64_MAX)pauseStarted=simulatedTime;
  return connected && (pauseStarted==UINT64_MAX || simulatedTime-pauseStarted>=pauseDuration);
 }
 operator bool() { return ready(); }
 int availableForWrite() { return ready()?2048:0; }
 size_t write(const uint8_t* data,size_t size) {
  if(!ready())return size; // HWCDC 在未连接分支报告 size，不能把它当作已交付。
  const size_t count=simulatedTime<zeroWriteUntil?0:std::min(size,maximumWrite);output.append((const char*)data,count);return count;
 }
 void print(const char* s) { write((const uint8_t*)s,strlen(s)); }
 void println(const char* s) { print(s);print("\n"); }
 void printf(const char* f, ...) { char b[2048]; va_list a; va_start(a,f); vsnprintf(b,sizeof(b),f,a); va_end(a); print(b); }
} inline Serial;
struct EspMock { uint64_t getEfuseMac(){return 123;} } inline ESP;
constexpr int PT_U8 = 1;
struct Preferences {
 inline static std::map<std::string,std::string> values;
 inline static bool failOpen=false, failWrite=false, failRead=false, invalidType=false;
 bool opened=false;
 bool begin(const char*,bool){if(opened||failOpen)return false;opened=true;return true;}
 bool isKey(const char* k){return values.count(k)>0;}
 int getType(const char*){return invalidType?0:PT_U8;}
 uint8_t getUChar(const char* k,uint8_t d){return failRead?d:values.count(k)?uint8_t(std::stoul(values[k])):d;}
 size_t putBool(const char* k,bool v){if(failWrite)return 0;values[k]=v?"1":"0";return 1;}
 bool getBool(const char* k,bool d){return values.count(k)?values[k]=="1":d;}
 size_t putUInt(const char* k,uint32_t v){values[k]=std::to_string(v);return 4;}
 uint32_t getUInt(const char* k,uint32_t d){return values.count(k)?std::stoul(values[k]):d;}
 size_t putString(const char* k,const char* v){values[k]=v;return strlen(v);}
 String getString(const char* k,const char* d){return values.count(k)?values[k]:std::string(d);}
 bool remove(const char* k){return values.erase(k)>0;}
};
struct File {
 struct Node {std::string path,content; bool directory=false;};
 inline static std::map<std::string,std::shared_ptr<Node>> nodes;
 inline static bool failWrite=false;
 std::shared_ptr<Node> node; size_t cursor=0,child=0;
 std::vector<std::shared_ptr<Node>> children;
 File()=default;
 File(std::shared_ptr<Node> n):node(n){ if(n->directory) for(auto& p:nodes) if(!p.second->directory) children.push_back(p.second); }
 operator bool()const{return bool(node);}
 bool isDirectory(){return node->directory;}
 const char* name(){return node->path.c_str()+node->path.find_last_of('/')+1;}
 bool available(){return cursor<node->content.size();}
 int read(){return available()?uint8_t(node->content[cursor++]):-1;}
 size_t size(){return node->content.size();}
 size_t write(const uint8_t* b,size_t n){size_t count=failWrite?n/2:n;node->content.append((const char*)b,count);return count;}
 void flush(){}
 int getWriteError(){return failWrite?1:0;}
 void close(){}
 File openNextFile(){return child<children.size()?File(children[child++]):File();}
};
#define FILE_APPEND "a"
struct Filesystem {
 bool full=false, formatted=true; unsigned formatCalls=0;
 bool begin(bool format){assert(!format);return formatted;}
 bool format(){++formatCalls;File::nodes.clear();formatted=true;return true;}
 bool exists(const char* p){return File::nodes.count(p)>0;}
 bool mkdir(const char* p){auto n=std::make_shared<File::Node>();n->path=p;n->directory=true;File::nodes[p]=n;return true;}
 size_t totalBytes(){return 14000000;}
 size_t usedBytes(){if(full)return totalBytes();size_t n=0;for(auto& p:File::nodes)n+=p.second->content.size();return n;}
 File open(const char* p,const char* mode="r"){
  if(!exists(p)){if(strcmp(mode,"a"))return {};auto n=std::make_shared<File::Node>();n->path=p;File::nodes[p]=n;}
  return File(File::nodes[p]);
 }
} inline LittleFS;
struct WireMock {
 bool connected=true, ready=false, badCrc=false; uint16_t instruction=0; unsigned pos=0;
 std::vector<uint8_t> response;
 bool begin(int,int,unsigned){return connected;}
 void end(){} void setTimeOut(unsigned){}
 void beginTransmission(unsigned){instruction=0;}
 void write(uint8_t b){instruction=uint16_t((instruction<<8)|b);}
 int endTransmission(){return connected?0:1;}
 unsigned requestFrom(uint8_t,uint8_t size){
  response.clear();pos=0;
  auto word=[&](uint16_t n){uint8_t a=n>>8,b=n,crc=0xff;for(auto v:{a,b}){crc^=v;for(int i=0;i<8;i++)crc=(crc&128)?uint8_t((crc<<1)^0x31):uint8_t(crc<<1);} response.insert(response.end(),{a,b,uint8_t(crc^(badCrc?1:0))});};
  if(instruction==0xe4b8)word(ready?1:0);else{word(950);word(27000);word(40000);}
  return response.size()==size?size:0;
 }
 bool available(){return pos<response.size();}
 int read(){return response[pos++];}
} inline Wire;
'''

NATIVE = r'''
#include "sensor.h"
#include "clock.h"
#include "prediction_settings_under_test.h"
void verifyPredictionDiagnostics(){
 Data::clear();Data::live=true;Data::current={800,25,50,20,true};
 unsigned inferenceCalls=0;
 static unsigned calls=0;
 Prediction::begin([](const float*,bool valid){assert(valid);++calls;Prediction::Result r;r.status=Prediction::Status::Success;r.nearConcentration=810;r.farConcentration=820;return r;});
 for(unsigned minute=0;minute<20;++minute)Data::append({800,25,50,minute,true,12,0,Data::MinuteFailure::None});
 assert(Data::historyDiagnostics().ready && Data::historyDiagnostics().continuousValidMinutes==20);
 Prediction::setEnabled(true);Prediction::update();inferenceCalls=calls;Prediction::update();assert(calls==inferenceCalls);
 Application::loadPredictionSettings();assert(Prediction::enabled);
 const auto retainedCount=Data::count;
 Application::togglePrediction();assert(!Prediction::enabled && Prediction::settingSaved);
 assert(Prediction::result.status==Prediction::Status::Disabled && !isfinite(Prediction::result.nearConcentration));
 Prediction::update();assert(calls==inferenceCalls && Data::count==retainedCount);
 Application::predictionSettings.opened=false;Application::loadPredictionSettings();assert(!Prediction::enabled);
 Application::togglePrediction();assert(Prediction::enabled && Prediction::result.status==Prediction::Status::Success);
 Preferences::failWrite=true;Application::togglePrediction();
 assert(!Prediction::enabled && !Prediction::settingSaved && strlen(Prediction::settingError)>0);
 assert(Prediction::result.status==Prediction::Status::Disabled && !isfinite(Prediction::result.farConcentration));
 Preferences::failWrite=false;Application::togglePrediction();assert(Prediction::settingSaved);
 Preferences::invalidType=true;Application::predictionSettings.opened=false;Application::loadPredictionSettings();assert(!Prediction::settingSaved);
 Preferences::invalidType=false;Preferences::failRead=true;Application::predictionSettings.opened=false;Application::loadPredictionSettings();assert(!Prediction::settingSaved);
 Preferences::failRead=false;Preferences::failOpen=true;Application::predictionSettings.opened=false;Application::loadPredictionSettings();assert(!Prediction::settingSaved);
 Preferences::failOpen=false;
 auto& bad=Data::history[Configuration::historyCapacity-11];bad.valid=false;bad.failureReason=Data::MinuteFailure::CrcFailed;bad.errorCount=1;
 auto diagnostic=Data::historyDiagnostics();
 assert(!diagnostic.ready && diagnostic.continuousValidMinutes==10 && diagnostic.firstBadMinute==9 && diagnostic.firstBadErrors==1 && diagnostic.firstBadSamples==12);
 assert(diagnostic.firstBadReason==Data::MinuteFailure::CrcFailed && diagnostic.firstBadOffsetMinutes==10);
 Prediction::setEnabled(true);Prediction::update();assert(Prediction::result.status==Prediction::Status::InvalidHistory && !isfinite(Prediction::result.nearConcentration));
 for(unsigned minute=20;minute<29;++minute)Data::append({800,25,50,minute,true,12,0});
 assert(!Data::historyDiagnostics().ready);Data::append({800,25,50,29,true,12,0});assert(Data::historyDiagnostics().ready);
 Data::history[Configuration::historyCapacity-3].minute+=1;assert(!Data::historyDiagnostics().ready);
 Data::clear();Data::append({800,25,50,0,true,8,0});assert(!Data::historyDiagnostics().ready);
 // 相同质量门槛下保留坏分钟的计数，不会因下一条成功读数抹去错误。
 Sensor::resetBucket();Sensor::bucketMinute=0;Sensor::bucketStarted=0;Sensor::started=false;
 Sensor::samples=8;Sensor::concentrationSum=6400;Sensor::temperatureSum=200;Sensor::humiditySum=400;
 simulatedTime=59000;Sensor::fail(Sensor::Status::CrcFailed);
 Sensor::lastRecovery=60000;simulatedTime=60000;Sensor::tick(simulatedTime);
 assert(!Data::history[Configuration::historyCapacity-1].valid);
 assert(Data::history[Configuration::historyCapacity-1].sampleCount==8 && Data::history[Configuration::historyCapacity-1].errorCount==1);
 assert(Data::history[Configuration::historyCapacity-1].failureReason==Data::MinuteFailure::CrcFailed);
 assert(Sensor::samples==0 && Sensor::bucketErrors==0);
 Sensor::samples=7;Sensor::bucketStarted=60000;Sensor::lastRecovery=120000;simulatedTime=120000;Sensor::tick(simulatedTime);
 assert(Data::history[Configuration::historyCapacity-1].failureReason==Data::MinuteFailure::TooFewSamples);
 Sensor::samples=8;Sensor::concentrationSum=6400;Sensor::temperatureSum=200;Sensor::humiditySum=400;
 Sensor::lastRecovery=300000;simulatedTime=300000;Sensor::tick(simulatedTime);
 assert(Data::history[Configuration::historyCapacity-3].valid);
 assert(Data::history[Configuration::historyCapacity-1].failureReason==Data::MinuteFailure::MissingMinute);
 puts("PASS: prediction NVS/default/reboot/failure, disabled CSV, immediate clearing, retained history, minute diagnostics, inference deduplication, unchanged quality gates");
}
bool verifyUsbExport(const char* scenario){
 Serial.output.clear();Serial.maximumWrite=2048;Serial.pauseAt=SIZE_MAX;Serial.pauseStarted=UINT64_MAX;Serial.pauseDuration=0;
 Serial.pauseOnText.clear();Serial.zeroWriteUntil=0;
 const bool stopped=strstr(scenario,"stopped")!=nullptr;
 if(stopped)Recording::stop();
 const bool wasActive=Recording::active(), intent=Recording::resumeRequested;
 std::map<std::string,std::string> preserved;
 for(const auto& item:File::nodes)preserved[item.first]=item.second->content;
 const auto started=simulatedTime;
 if(!strcmp(scenario,"transient") || stopped){Serial.pauseAt=100;Serial.pauseDuration=30;}
 if(!strcmp(scenario,"short_write"))Serial.maximumWrite=7;
 if(!strcmp(scenario,"write_recovery"))Serial.zeroWriteUntil=simulatedTime+30;
 if(!strcmp(scenario,"header_recovery")){Serial.pauseOnText="recording_session";Serial.pauseDuration=30;}
 if(!strcmp(scenario,"end_recovery")){Serial.pauseOnText="recording_export_end";Serial.pauseDuration=30;}
 const bool stalled=!strcmp(scenario,"stalled"), disconnected=!strcmp(scenario,"disconnected");
 if(stalled)Serial.maximumWrite=0;
 if(disconnected){Serial.pauseAt=100;Serial.pauseDuration=UINT32_MAX;}
 Recording::exportAll();
 const bool ended=Serial.output.find("recording_export_end")!=std::string::npos;
 bool passed=false;
 if(stalled || disconnected){
  passed=!ended && simulatedTime>started && simulatedTime-started<10000;
  passed=passed && File::nodes[Recording::currentPath]->content.find(",export_aborted,usb_export_timeout")!=std::string::npos;
  passed=passed && File::nodes[Recording::currentPath]->content.find(",export_end,")==std::string::npos;
 } else {
  passed=ended && Serial.output.find("recording_row,")!=std::string::npos && Serial.output.find("recording_session")!=std::string::npos;
  const std::string path=std::string("protocol-")+scenario+".log";
  FILE* trace=nullptr;assert(fopen_s(&trace,path.c_str(),"wb")==0);
  fwrite(Serial.output.data(),1,Serial.output.size(),trace);fclose(trace);
 }
 for(const auto& item:preserved)passed=passed && File::nodes[item.first]->content.find(item.second)==0;
 passed=passed && Recording::active()==wasActive && Recording::resumeRequested==intent;
 if(!passed)fprintf(stderr,"FAIL USB %s: end=%d elapsed=%llu output=%zu\n",scenario,ended,(unsigned long long)(simulatedTime-started),Serial.output.size());
 else printf("PASS USB %s: elapsed=%llu\n",scenario,(unsigned long long)(simulatedTime-started));
 return passed;
}
int main(int argc,char** argv){
 // 复现首次存储未格式化，用户初始化后立即开始记录的真实调用顺序。
 LittleFS.formatted=false;
 assert(!Recording::begin()); assert(Recording::settingsReady); assert(LittleFS.formatCalls==0);
 assert(Recording::initialize()); assert(LittleFS.formatCalls==1);
 assert(!Recording::resumeRequested);
 Recording::selectEnvironment(1);
 Recording::Session session{Recording::Source::ClassroomMeasurement,"","model",0};
 if(!Recording::start(session)) {
  fputs("FAIL: initialize succeeded but recording cannot start\n",stderr);return 1;
 }
 assert(Recording::append({{850,24,50,0,true},1000},{}));
 assert(Recording::appendForecast({1000,0,nullptr,5,15,910,870,910,870,850,800,0,"model","fan_assumption=nominal"}));
 const std::string firstSaved=File::nodes[Recording::currentPath]->content;
 assert(!Recording::initialize());assert(LittleFS.formatCalls==1);
 assert(File::nodes[Recording::currentPath]->content==firstSaved);
 Sensor::begin();
 Data::append({900,25,60,0,true});
 const auto count=Data::count, identity=Recording::sessionId;
 simulatedTime=16000; Sensor::tick(simulatedTime);
 assert(Sensor::status==Sensor::Status::Stale);
 assert(Recording::active() && Data::count==count);
 simulatedTime=31000; Sensor::tick(simulatedTime);
 assert(Sensor::recovering && Recording::sessionId==identity && Data::count==count);
 simulatedTime=31500;Sensor::tick(simulatedTime);assert(Sensor::started);
 Wire.ready=true;simulatedTime=33000;Sensor::tick(simulatedTime);
 assert(Sensor::measurementReady && Sensor::status==Sensor::Status::Ready);
 assert(Recording::append({Data::current,simulatedTime},{}));
 Wire.ready=false;simulatedTime=34000;Sensor::tick(simulatedTime);assert(!Sensor::measurementReady);
 Wire.badCrc=true;simulatedTime=35000;Sensor::tick(simulatedTime);assert(Sensor::status==Sensor::Status::CrcFailed);
 Wire.badCrc=false;
 const std::string originalPath=Recording::currentPath;
 const std::string saved=File::nodes[originalPath]->content;
 assert(saved.find(",sensor_fault,stale")!=std::string::npos);
 assert(saved.find(",sensor_recovered,")!=std::string::npos);
 assert(saved.find(",measurement,\n")!=std::string::npos);
 assert(saved.find(",1000,0,,0,5,15,910.000,870.000,910.000,870.000,850.000,800.000,0,model,fan_assumption=nominal")!=std::string::npos);
 if(argc>1)return verifyUsbExport(argv[1])?0:1;
 Recording::exportAll();assert(Recording::active());
 assert(Serial.output.find("recording_export_end")!=std::string::npos);
 // 模拟启动重载：保留旧文件，恢复用户意图并新建场次。
 assert(Recording::begin() && Recording::resumeRequested && Recording::selectedEnvironment==1);
 assert(Recording::start(session));assert(Recording::sessionId==identity+1);
 assert(File::nodes[originalPath]->content.find(saved)==0);
 LittleFS.full=true;assert(!Recording::event("heartbeat"));assert(Recording::status==Recording::Status::Full);
 assert(Preferences::values["last_failure"]=="storage_full");
 LittleFS.full=false;assert(Recording::begin());assert(Recording::start(session));
 assert(File::nodes[Recording::currentPath]->content.find("previous_storage_failure,storage_full")!=std::string::npos);
 File::failWrite=true;assert(!Recording::event("heartbeat"));assert(Recording::status==Recording::Status::WriteFailed);
 File::failWrite=false;assert(Recording::begin());assert(Recording::start(session));
 Recording::stop();assert(!Recording::resumeRequested);assert(Recording::begin() && !Recording::resumeRequested);
 Serial.output.clear();Recording::exportAll();
 FILE* trace=nullptr;assert(fopen_s(&trace,"protocol.log","wb")==0);
 fwrite(Serial.output.data(),1,Serial.output.size(),trace);fclose(trace);
 simulatedTime=0;assert(Clock::setFromIso("2026-10-11T12:00:00+08:00"));
 const auto epoch=Clock::epochMilliseconds();simulatedTime=(1ULL<<32)+1000;
 assert(Clock::epochMilliseconds()==epoch+simulatedTime);
 verifyPredictionDiagnostics();
 puts("PASS: initialize then record, repeated begin, initialized storage protection, stale/recovery/history preservation, CRC faults, raw measurement, resume, export continuity, full/partial write preservation, manual stop");
}
'''

def native_test():
    directory = ROOT / '.build' / 'recording-reliability-tests'
    directory.mkdir(parents=True, exist_ok=True)
    (directory/'stub.h').write_text(STUB, encoding='utf-8')
    for name in ('Arduino.h','LittleFS.h','Preferences.h','esp_timer.h','esp_system.h','Wire.h'):
        (directory/name).write_text('#include "stub.h"\n', encoding='utf-8')
    (directory/'verify.cpp').write_text(NATIVE, encoding='utf-8')
    application = (ROOT/'firmware/application.h').read_text(encoding='utf-8')
    settings = application[application.index('inline Preferences predictionSettings;'):application.index('inline Prediction::Result runModel')]
    toggle = application[application.index('inline void togglePrediction()'):application.index('inline void reportStatus()')]
    (directory/'prediction_settings_under_test.h').write_text(
        'namespace Application { inline uint32_t lastForecastMinute=UINT32_MAX;\n'
        'inline void recordForecastSnapshot() {} inline void logPrediction() {}\n'+settings+toggle+'}\n', encoding='utf-8')
    vswhere = Path(os.environ['ProgramFiles(x86)'])/'Microsoft Visual Studio/Installer/vswhere.exe'
    installation = subprocess.check_output([str(vswhere),'-products','*','-latest','-property','installationPath'],text=True).strip()
    setup = Path(installation)/'VC/Auxiliary/Build/vcvars64.bat'
    batch = directory/'verify.cmd'
    batch.write_text(f'@echo off\ncall "{setup}" >nul\ncl /nologo /std:c++17 /EHsc /utf-8 /I. /I"{ROOT / "firmware"}" verify.cpp /Fe:verify.exe\nif errorlevel 1 exit /b 1\nverify.exe\n',encoding='utf-8')
    subprocess.run(['cmd','/c',str(batch)],cwd=directory,check=True)
    failures=[]
    for scenario in ('transient','short_write','stalled','disconnected','write_recovery','header_recovery','end_recovery','transient_stopped'):
        result=subprocess.run([str(directory/'verify.exe'),scenario],cwd=directory)
        if result.returncode:failures.append(scenario)
        elif scenario not in ('stalled','disconnected'):
            exporter.read_export((directory/f'protocol-{scenario}.log').read_text(encoding='utf-8').splitlines(),
                                 csv.DictWriter(io.StringIO(), exporter.CSV_FIELDS))
    if failures:raise AssertionError(f'USB export regressions failed: {failures}')
    target = io.StringIO()
    result = exporter.read_export((directory/'protocol.log').read_text(encoding='utf-8').splitlines(),
                                  csv.DictWriter(target, exporter.CSV_FIELDS))
    assert result['incomplete_rows'] == 1, result
    rows = list(csv.DictReader(io.StringIO(target.getvalue())))
    assert any(row['record_type'] == 'measurement' and row['co2_ppm'] == '950.000' for row in rows)
    assert any(row['error_code'] == 'crc_failed' for row in rows)
    print('PASS: native firmware export parsed by Python; partial tail reported, valid data preserved')

if __name__ == '__main__':
    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(ExportTests))
    if not result.wasSuccessful(): raise SystemExit(1)
    native_test()
