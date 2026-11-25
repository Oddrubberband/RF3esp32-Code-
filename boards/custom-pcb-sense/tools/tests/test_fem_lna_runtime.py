"""Exercise production LNA HAL and command guards using controlled host adapters."""
import argparse
import os
from pathlib import Path
import subprocess

def method(source, signature):
    start = source.index(signature)
    body = source.index('{', start)
    end = body + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--compiler', default=r'C:\msys64\ucrt64\bin\g++.exe')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    custom = 'femAntenna()' in (root/'include/esp32_nrf24_hal.hpp').read_text()
    out = args.output.resolve()
    (out/'driver').mkdir(parents=True, exist_ok=True)
    gpio = r"""
#pragma once
#include <cassert>
#include <cstdint>
#include <map>
using gpio_num_t=int;
using gpio_mode_t=int;
constexpr int GPIO_NUM_32=32, GPIO_NUM_33=33, GPIO_NUM_14=14, GPIO_NUM_13=13, GPIO_NUM_17=17;
constexpr int GPIO_MODE_OUTPUT=1,GPIO_MODE_INPUT=0,GPIO_PULLUP_DISABLE=0,GPIO_PULLDOWN_DISABLE=0,GPIO_INTR_DISABLE=0;
struct gpio_config_t{uint64_t pin_bit_mask=0;int mode=0,pull_up_en=0,pull_down_en=0,intr_type=0;};
inline std::map<int,int> levels;
inline int gpio_set_level(int pin,int value){levels[pin]=value;assert(!(levels[32]&&levels[33]));return 0;}
inline int gpio_config(const gpio_config_t*){return 0;}
inline int gpio_set_direction(int,int){return 0;}
inline int gpio_get_level(int pin){return levels[pin];}
"""
    spi = r"""
#pragma once
#include <cstddef>
#include <cstdint>
using spi_host_device_t=int;
using spi_device_handle_t=void*;
constexpr int SPI3_HOST=3,SPI_DMA_CH_AUTO=0;
struct spi_bus_config_t{int sclk_io_num=0,mosi_io_num=0,miso_io_num=0,quadwp_io_num=0,quadhd_io_num=0;};
struct spi_device_interface_config_t{int clock_speed_hz=0,mode=0,spics_io_num=0,queue_size=0;};
struct spi_transaction_t{uint32_t length=0;const void* tx_buffer=nullptr;void* rx_buffer=nullptr;};
inline int spi_bus_initialize(int,const spi_bus_config_t*,int){return 0;}
inline int spi_bus_add_device(int,const spi_device_interface_config_t*,spi_device_handle_t* h){*h=reinterpret_cast<void*>(1);return 0;}
inline int spi_device_transmit(spi_device_handle_t,spi_transaction_t*){return 0;}
"""
    (out/'driver/gpio.h').write_text(gpio)
    (out/'driver/spi_master.h').write_text(spi)
    (out/'esp_err.h').write_text('#pragma once\n#include <cassert>\nconstexpr int ESP_OK=0;\n#define ESP_ERROR_CHECK(x) assert((x)==ESP_OK)\n')
    (out/'esp_rom_sys.h').write_text('#pragma once\n#include <cstdint>\ninline uint64_t elapsed=0;\ninline void esp_rom_delay_us(unsigned us){elapsed+=us;}\n')
    (out/'esp_timer.h').write_text('#pragma once\n#include "esp_rom_sys.h"\ninline int64_t esp_timer_get_time(){return elapsed;}\n')
    source = (root/'src/main.cpp').read_text()
    cpp=r"""
#include <atomic>
#include <cctype>
#include <cstdio>
#include <string>
#include <vector>
#include "esp32_nrf24_hal.hpp"
#include "host_status_protocol.hpp"
using TickType_t=unsigned;
#define pdMS_TO_TICKS(v) (v)
enum class CommandOrigin {Local, Bluetooth, Http, Remote};
enum class RadioState {Standby, RxListening, TxBusy, Cw, Sleep, Fault};
std::string uppercaseCopy(std::string s){for(char& c:s)c=static_cast<char>(std::toupper(static_cast<unsigned char>(c)));return s;}
struct Manager {struct Status{RadioState state=RadioState::Standby;} current;Status status(){return current;}};
struct App {
    explicit App(Esp32Nrf24Hal& hal):hal_(hal){}
    Esp32Nrf24Hal& hal_;
    struct Loop {bool active=false;} loop_config_;
    std::atomic_bool loop_owns_radio_{false};
    Manager manager_;
    bool loop_available=true,radio_available=true,incoming=false;
    unsigned loop_locks=0,radio_locks=0;
    bool takeLoop(unsigned){if(!loop_available)return false;++loop_locks;return true;}
    bool takeRadio(unsigned){if(!radio_available)return false;++radio_locks;return true;}
    void giveLoop(){assert(loop_locks==1);--loop_locks;}
    void giveRadio(){assert(radio_locks==1);--radio_locks;}
    bool hasActiveIncomingReceiveLocked(){return incoming;}
METHOD
};
int main(){
    Esp32Nrf24Config cfg;
    Esp32Nrf24Hal hal(cfg);
    assert(std::string(hal.femLna())=="bypass");
    App app{hal};
    auto command=[&](std::vector<std::string> words,CommandOrigin origin=CommandOrigin::Local){
        bool result=app.commandFem(words,origin);
        assert(app.loop_locks==0&&app.radio_locks==0);
        return result;
    };
    constexpr int cps=CPS,csd=CSD;
    hal.frontEnd(FrontEndMode::Rx);
    assert(!levels[32]&&levels[33]&&!levels[cps]&&levels[csd]);
    assert(!hal.selectFemLna(false));
    hal.frontEnd(FrontEndMode::Tx);
    assert(levels[32]&&!levels[33]&&levels[cps]&&levels[csd]);
    hal.frontEnd(FrontEndMode::Rx);
    assert(!levels[32]&&levels[33]&&!levels[cps]&&levels[csd]);
    hal.frontEnd(FrontEndMode::Off);
    assert(!levels[32]&&!levels[33]&&!levels[cps]&&!levels[csd]);
    for(auto state:{RadioState::RxListening,RadioState::TxBusy,RadioState::Cw,RadioState::Sleep,RadioState::Fault}){
        app.manager_.current.state=state;assert(!command({"FEM","LNA","GAIN"}));
        assert(std::string(hal.femLna())=="bypass");
    }
    app.manager_.current.state=RadioState::Standby;
    app.incoming=true;assert(!command({"FEM","LNA","GAIN"}));app.incoming=false;
    app.loop_config_.active=true;assert(!command({"FEM","LNA","GAIN"}));app.loop_config_.active=false;
    app.loop_owns_radio_=true;assert(!command({"FEM","LNA","GAIN"}));app.loop_owns_radio_=false;
    app.loop_available=false;assert(!command({"FEM","LNA","GAIN"}));app.loop_available=true;
    app.radio_available=false;assert(!command({"FEM","LNA","GAIN"}));app.radio_available=true;
    for(auto origin:{CommandOrigin::Http,CommandOrigin::Remote})assert(!command({"FEM","LNA","GAIN"},origin));
    for(auto words:{std::vector<std::string>{"FEM"},{"FEM","LNA"},{"FEM","LNA","invalid"},{"FEM","LNA","GAIN","extra"}})assert(!command(words));
    hal.ce(true);assert(!command({"FEM","LNA","GAIN"}));hal.ce(false);
    assert(hal.setSignalOutputsEnabled(false));
    assert(!command({"FEM","LNA","GAIN"}));
    hal.frontEnd(FrontEndMode::Rx);
    assert(!levels[32]&&!levels[33]&&!levels[cps]&&!levels[csd]);
    assert(std::string(hal.femLna())=="bypass");
    assert(hal.setSignalOutputsEnabled(true));
    assert(command({"FEM","LNA","GAIN"},CommandOrigin::Bluetooth));
    assert(std::string(hal.femLna())=="gain");
    assert(!levels[32]&&!levels[33]&&!levels[cps]&&!levels[csd]);
    hal.frontEnd(FrontEndMode::Rx);
    assert(!levels[32]&&levels[33]&&levels[cps]&&levels[csd]);
    hal.frontEnd(FrontEndMode::Tx);
    assert(levels[32]&&!levels[33]&&levels[cps]&&levels[csd]);
    hal.frontEnd(FrontEndMode::Rx);
    assert(!levels[32]&&levels[33]&&levels[cps]&&levels[csd]);
    hal.frontEnd(FrontEndMode::Off);
    assert(!levels[32]&&!levels[33]&&!levels[cps]&&!levels[csd]);
    assert(command({"FEM","LNA","BYPASS"}));
    hal.frontEnd(FrontEndMode::Rx);
    assert(!levels[32]&&levels[33]&&!levels[cps]&&levels[csd]);
    hal.frontEnd(FrontEndMode::Off);
    HostStatusProtocol::Snapshot status;
    assert(HostStatusProtocol::json(status).find("\"fem_lna\"")==std::string::npos);
    for(const char* lna:{"gain","bypass"}){
        status.fem_lna=lna;
        assert(HostStatusProtocol::json(status).find(std::string("\"fem_lna\":\"")+lna+"\"")!=std::string::npos);
    }
    status.fem_lna="invalid";
    assert(HostStatusProtocol::json(status).find("\"fem_lna\"")==std::string::npos);
    std::puts("PASS production HAL RX bypass/gain, TX PA, turnaround, cutoff/recovery, command validation/locks/origins/ownership, status serialization");
}
""".replace('METHOD',method(source,'    bool commandFem(')).replace('CPS',str(25 if custom else 13)).replace('CSD',str(26 if custom else 14))
    path=out/'fem_lna.cpp';path.write_text(cpp)
    flags=['-DRF3_AUTOMATIC_FEM=1','-DNRF24_SCK_PIN=18','-DNRF24_MOSI_PIN=23','-DNRF24_MISO_PIN=19']
    flags+=['-DRF3_HARDWARE_PROFILE_ID=1','-DNRF24_CE_PIN=17','-DNRF24_CSN_PIN=27','-DNRF24_IRQ_PIN=16','-DRF3_FEM_CTX_PIN=32','-DRF3_FEM_CRX_PIN=33','-DRF3_FEM_CSD_PIN=26','-DRF3_FEM_CPS_PIN=25','-DRF3_FEM_ANT_PIN=21'] if custom else ['-DRF3_HARDWARE_PROFILE_ID=2','-DNRF24_CE_PIN=27','-DNRF24_CSN_PIN=5','-DNRF24_IRQ_PIN=26']
    env=os.environ.copy();env['PATH']=str(Path(args.compiler).parent)+os.pathsep+env['PATH']
    binary=out/'fem_lna.exe'
    subprocess.run([args.compiler,'-std=c++17','-Wall','-Wextra','-Werror','-pedantic',*flags,'-I',str(out),'-I',str(root/'include'),str(path),str(root/'src/esp32_nrf24_hal.cpp'),'-o',str(binary)],env=env,check=True)
    subprocess.run([str(binary)],env=env,check=True)
    disabled = out/'fem_lna_disabled.cpp'
    disabled.write_text('#include <cassert>\n#include "esp32_nrf24_hal.hpp"\nint main(){Esp32Nrf24Config cfg;Esp32Nrf24Hal hal(cfg);assert(hal.femLna()==nullptr);assert(!hal.selectFemLna(true));assert(!hal.selectFemLna(false));}\n')
    disabled_binary = out/'fem_lna_disabled.exe'
    disabled_flags = [flag for flag in flags if flag != '-DRF3_AUTOMATIC_FEM=1']
    subprocess.run([args.compiler,'-std=c++17','-Wall','-Wextra','-Werror','-pedantic',*disabled_flags,'-DRF3_AUTOMATIC_FEM=0','-I',str(out),'-I',str(root/'include'),str(disabled),str(root/'src/esp32_nrf24_hal.cpp'),'-o',str(disabled_binary)],env=env,check=True)
    subprocess.run([str(disabled_binary)],env=env,check=True)
    print('PASS FEM-disabled build rejects selection and omits capability')

if __name__=='__main__':main()
