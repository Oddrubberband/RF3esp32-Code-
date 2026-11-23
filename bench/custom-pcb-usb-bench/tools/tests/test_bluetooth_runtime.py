"""Execute production SPP lifecycle and enqueue methods with bounded task adapters."""
import argparse
import os
from pathlib import Path
import subprocess

def method(source, signature):
    start=source.index(signature)
    brace=source.index('{', start)
    depth=1
    end=brace+1
    while depth:
        depth += (source[end]=='{') - (source[end]=='}')
        end+=1
    return source[start:end]

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--compiler',default=r'C:\msys64\ucrt64\bin\g++.exe')
    args=parser.parse_args()
    root=Path(__file__).resolve().parents[2]
    source=(root/'src/main.cpp').read_text()
    event=source[source.index('            case ESP_SPP_SRV_OPEN_EVT:'):source.index('            case ESP_SPP_WRITE_EVT:')]
    cpp=r"""
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <string_view>
#include <vector>
#include "bluetooth_tx_flow.hpp"
using esp_err_t = int;
constexpr int ESP_OK=0, ESP_SPP_SUCCESS=0,pdTRUE=1;
constexpr uint32_t kBluetoothDisconnectRetryMs=250;
constexpr int ESP_SPP_SRV_OPEN_EVT=1,ESP_SPP_CLOSE_EVT=2,ESP_SPP_DATA_IND_EVT=3;
constexpr int64_t kBluetoothWriteTimeoutUs=8000000;
constexpr const char* TAG="test";
#define portENTER_CRITICAL(x) do {(void)(x);} while(false)
#define portEXIT_CRITICAL(x) do {(void)(x);} while(false)
#define ESP_LOGI(...) do {} while(false)
#define ESP_LOGW(...) do {} while(false)
struct BluetoothTxMessage{uint32_t handle=0,epoch=0;uint16_t length=0;char bytes[2048]{};};
struct Queue{std::deque<BluetoothTxMessage> items;};
int resets=0,delays=0,drain_after=0,disconnect_after=0;
int64_t clock_us=0;
void* current_task=nullptr;
std::vector<uint32_t> disconnected;
void* xTaskGetCurrentTaskHandle(){return current_task;}
int64_t esp_timer_get_time(){return clock_us;}
int xQueueSend(Queue* q,const BluetoothTxMessage* m,int){if(q->items.size()>=8)return 0;q->items.push_back(*m);return pdTRUE;}
void xQueueReset(Queue* q){q->items.clear();++resets;}
esp_err_t esp_spp_disconnect(uint32_t handle){disconnected.push_back(handle);return ESP_OK;}
struct Event {struct{int status=0;uint32_t handle=0;}srv_open,close;struct{uint32_t handle=0;uint8_t* data=nullptr;uint16_t len=0;}data_ind;};
class App{
public:
    Queue command,tx;
    Queue* bluetooth_command_queue_=&command;
    Queue* bluetooth_tx_queue_=&tx;
    void* bluetooth_command_task_=reinterpret_cast<void*>(1);
    std::atomic_bool bluetooth_client_connected_{false};
    std::atomic<uint32_t> bluetooth_spp_handle_{0},bluetooth_epoch_{0};
    int bluetooth_session_mux_=0;
    bool bluetooth_disconnect_pending_=false,bluetooth_close_received_=false;
    uint32_t bluetooth_retry_handle_=0, bluetooth_retry_epoch_=0;
    int64_t bluetooth_retry_at_us_=0;
    BluetoothTx::Flow bluetooth_tx_flow_;
    std::string bluetooth_rx_line_;
    bool bluetooth_rx_overflow_=false;
    unsigned received=0;
    void handleBluetoothData(const uint8_t*,uint16_t){++received;}
    void delayAtLeastMs(unsigned milliseconds){
        ++delays;clock_us+=milliseconds*1000;
        if(drain_after && delays>=drain_after && !tx.items.empty())tx.items.pop_front();
        if(disconnect_after && delays>=disconnect_after)bluetooth_client_connected_=false;
    }
    void event(int event,Event* param){switch(event){
"""+event+r"""
    default:break;
    }}
"""+method(source,'    bool sendBluetoothText(')+method(source,'    void failBluetoothLink(')+r"""
};
int main(){
    App app;Event e;e.srv_open.handle=12;app.event(ESP_SPP_SRV_OPEN_EVT,&e);
    assert(app.bluetooth_client_connected_ && app.tx.items.size()==1);
    auto epoch=app.bluetooth_epoch_.load();
    app.failBluetoothLink(12,epoch);assert(!app.bluetooth_client_connected_ && app.bluetooth_spp_handle_==12);
    e.srv_open.handle=13;app.event(ESP_SPP_SRV_OPEN_EVT,&e);
    assert(app.bluetooth_spp_handle_==12 && disconnected.back()==13);
    e.data_ind.handle=12;app.event(ESP_SPP_DATA_IND_EVT,&e);assert(app.received==0);
    app.command.items.emplace_back();e.close.handle=12;app.event(ESP_SPP_CLOSE_EVT,&e);
    assert(app.tx.items.empty() && app.command.items.empty() && app.bluetooth_spp_handle_==0);
    e.srv_open.handle=13;app.event(ESP_SPP_SRV_OPEN_EVT,&e);
    assert(app.bluetooth_client_connected_ && app.bluetooth_spp_handle_==13);
    app.failBluetoothLink(12,epoch);assert(app.bluetooth_client_connected_);
    assert(!app.sendBluetoothText("old",epoch));
    app.tx.items.clear();for(int n=0;n<8;++n)app.tx.items.emplace_back();
    current_task=app.bluetooth_command_task_;drain_after=3;delays=0;
    assert(app.sendBluetoothText("reply",app.bluetooth_epoch_));
    assert(delays==3 && app.bluetooth_client_connected_);
    drain_after=0;delays=0;disconnect_after=2;
    assert(!app.sendBluetoothText("disconnected",app.bluetooth_epoch_));
    assert(delays==2);
    app.bluetooth_client_connected_=true;disconnect_after=0;delays=0;clock_us=0;
    assert(!app.sendBluetoothText("timeout",app.bluetooth_epoch_));
    assert(clock_us==kBluetoothWriteTimeoutUs && !app.bluetooth_client_connected_);
    app.bluetooth_client_connected_=true;current_task=nullptr;delays=0;
    assert(!app.sendBluetoothText("callback",app.bluetooth_epoch_));assert(delays==0);
    std::puts("Production SPP failed-close/open reservation, queue cleanup, stale epoch, ingress, bounded backpressure, disconnect and callback checks PASS");
}
"""
    out=args.output.resolve()
    out.mkdir(parents=True,exist_ok=True)
    path=out/'bluetooth_runtime.cpp'
    path.write_text(cpp)
    binary=out/'bluetooth_runtime.exe'
    env=os.environ.copy()
    env['PATH']=str(Path(args.compiler).parent)+os.pathsep+env['PATH']
    subprocess.run([args.compiler,'-std=c++17','-Wall','-Wextra','-Werror','-pedantic','-I',str(root/'include'),str(path),'-o',str(binary)],env=env,check=True)
    subprocess.run([str(binary)],env=env,check=True)

if __name__=='__main__':
    main()
