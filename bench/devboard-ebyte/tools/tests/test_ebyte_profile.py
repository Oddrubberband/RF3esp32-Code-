"""Compile production USB EBYTE HAL and both peer drivers without hardware access."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--custom-root', type=Path, required=True)
    parser.add_argument('--compiler', default=r'C:\msys64\ucrt64\bin\g++.exe')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    out = args.output.resolve()
    (out/'driver').mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env['PATH'] = str(Path(args.compiler).parent)+os.pathsep+env['PATH']
    flags = ['-DRF3_HARDWARE_PROFILE_ID=2','-DRF3_AUTOMATIC_FEM=0','-DRF3_USB_EBYTE=1',
             '-DNRF24_CE_PIN=27','-DNRF24_CSN_PIN=5','-DNRF24_IRQ_PIN=26',
             '-DNRF24_SCK_PIN=18','-DNRF24_MOSI_PIN=23','-DNRF24_MISO_PIN=19',
             '-DRF3_NRF24_TX_CE_PULSE_US=40']
    common = ['-std=c++17','-Wall','-Wextra','-Werror','-pedantic']
    gpio = r'''
#pragma once
#include <cassert>
#include <cstdint>
#include <map>
#include <set>
using gpio_num_t=int;
using gpio_mode_t=int;
constexpr int GPIO_MODE_OUTPUT=1,GPIO_MODE_INPUT=0,GPIO_PULLUP_DISABLE=0,GPIO_PULLDOWN_DISABLE=0,GPIO_INTR_DISABLE=0;
struct gpio_config_t{uint64_t pin_bit_mask=0;int mode=0,pull_up_en=0,pull_down_en=0,intr_type=0;};
inline std::set<int> touched;
inline std::map<int,int> levels;
inline bool allowed(int pin){return pin==27||pin==5||pin==26||pin==18||pin==23||pin==19;}
inline void touch(int pin){assert(allowed(pin));touched.insert(pin);}
inline int gpio_set_level(int pin,int value){touch(pin);levels[pin]=value;return 0;}
inline int gpio_config(const gpio_config_t* cfg){for(int pin=0;pin<64;++pin)if(cfg->pin_bit_mask&(1ULL<<pin))touch(pin);return 0;}
inline int gpio_set_direction(int pin,int){touch(pin);return 0;}
inline int gpio_get_level(int pin){touch(pin);return levels[pin];}
'''
    spi = r'''
#pragma once
#include <cstddef>
#include <cstdint>
#include "gpio.h"
using spi_host_device_t=int;
using spi_device_handle_t=void*;
constexpr int SPI3_HOST=3,SPI_DMA_CH_AUTO=0;
struct spi_bus_config_t{int sclk_io_num=0,mosi_io_num=0,miso_io_num=0,quadwp_io_num=0,quadhd_io_num=0;};
struct spi_device_interface_config_t{int clock_speed_hz=0,mode=0,spics_io_num=0,queue_size=0;};
struct spi_transaction_t{uint32_t length=0;const void* tx_buffer=nullptr;void* rx_buffer=nullptr;};
inline unsigned transactions=0;
inline int spi_bus_initialize(int,const spi_bus_config_t* cfg,int){assert(cfg->sclk_io_num==18&&cfg->mosi_io_num==23&&cfg->miso_io_num==19);touch(18);touch(23);touch(19);return 0;}
inline int spi_bus_add_device(int,const spi_device_interface_config_t* cfg,spi_device_handle_t* h){assert(cfg->spics_io_num==5&&cfg->clock_speed_hz==1000000);touch(5);*h=reinterpret_cast<void*>(1);return 0;}
inline int spi_device_transmit(spi_device_handle_t,spi_transaction_t*){++transactions;return 0;}
'''
    (out/'driver/gpio.h').write_text(gpio)
    (out/'driver/spi_master.h').write_text(spi)
    (out/'esp_err.h').write_text('#pragma once\n#include <cassert>\nconstexpr int ESP_OK=0;\n#define ESP_ERROR_CHECK(x) assert((x)==ESP_OK)\n')
    (out/'esp_rom_sys.h').write_text('#pragma once\n#include <cstdint>\ninline uint64_t elapsed=0;\ninline void esp_rom_delay_us(unsigned us){elapsed+=us;}\n')
    (out/'esp_timer.h').write_text('#pragma once\n#include "esp_rom_sys.h"\ninline int64_t esp_timer_get_time(){return elapsed;}\n')
    cpp = r'''
#include <cstdio>
#include <string>
#include "esp32_nrf24_hal.hpp"
#include "devboard_power.hpp"
#include "host_status_protocol.hpp"
struct QuietPower : DevboardPower::Io {
    unsigned outputs=0,adc_start=0,reads=0,loads=0,stores=0;
    bool output(int,bool)override{++outputs;return true;}
    bool beginAdc(int)override{++adc_start;return true;}
    DevboardPower::Reading readAdc()override{++reads;return {};}
    bool loadCutoff(bool&)override{++loads;return true;}
    bool storeCutoff(bool)override{++stores;return true;}
};
int main(){
    static_assert(HardwareProfile::kSelectedBatteryPower==HardwareProfile::BatteryPower::None);
    QuietPower io;DevboardPower::Controller power(io,HardwareProfile::kSelectedBatteryPower);
    assert(power.initialize()&&!power.supported()&&!power.hasNrfRailControl());
    assert(power.radioAvailable()&&power.allowRf(0));
    assert(!power.poll(100000,true)&&!power.activeCheckDue(100000));
    assert(!power.requestCutoff()&&!power.applyCutoff()&&!power.resetCutoff(100000));
    assert(!power.setNrfRail(false)&&!power.setNrfRail(true));
    assert(io.outputs==0&&io.adc_start==0&&io.reads==0&&io.loads==0&&io.stores==0);
    Esp32Nrf24Config cfg;Esp32Nrf24Hal hal(cfg);
    assert(!hal.ceAsserted()&&hal.femLna()==nullptr);
    assert(!hal.selectFemLna(true)&&!hal.selectFemLna(false));
    const auto initial_touched=touched;
    for(auto mode:{FrontEndMode::Rx,FrontEndMode::Tx,FrontEndMode::Off}){
        hal.frontEnd(mode);assert(touched==initial_touched);
    }
    hal.ce(true);assert(hal.ceAsserted()&&levels[27]);
    hal.ce(false);assert(!hal.ceAsserted()&&!levels[27]);
    uint8_t tx[2]{0xff,0xff},rx[2]{};
    hal.spiTxRx(tx,rx,2);assert(transactions==1);
    assert(hal.irqConnected());levels[26]=0;assert(hal.irqAsserted());levels[26]=1;assert(!hal.irqAsserted());
    assert(hal.setSignalOutputsEnabled(false));
    hal.ce(true);assert(!hal.ceAsserted()&&!levels[27]);
    hal.spiTxRx(tx,rx,2);assert(transactions==1&&rx[0]==0xff&&rx[1]==0xff);
    assert(hal.setSignalOutputsEnabled(true));
    hal.ce(true);assert(hal.ceAsserted());hal.ce(false);
    HostStatusProtocol::Snapshot status;status.fem_lna=hal.femLna();status.board_power=power.supported();
    const auto encoded=HostStatusProtocol::json(status);
    for(const char* field:{"fem_lna","fem_antenna","fem_mode","board_power","battery_mv","nrf_rail"})
        assert(encoded.find(std::string("\"")+field+"\"")==std::string::npos);
    std::puts("PASS actual no-FEM HAL constructor/SPI/CE/IRQ/signals, RX/TX/OFF no GPIO claims, no LNA capability and zero battery/ADC/NVS operations");
}
'''
    path=out/'ebyte_hal.cpp';path.write_text(cpp)
    binary=out/'ebyte_hal.exe'
    subprocess.run([args.compiler,*common,*flags,'-I',str(out),'-I',str(root/'include'),str(path),str(root/'src/esp32_nrf24_hal.cpp'),'-o',str(binary)],env=env,check=True)
    subprocess.run([str(binary)],env=env,check=True)
    results={}
    for label,peer,peer_flags in [('ebyte',root,flags),('custom',args.custom_root.resolve(),['-DRF3_AUTOMATIC_FEM=1','-DRF3_NRF24_TX_CE_PULSE_US=40'])]:
        binary=out/(label+'_turnaround.exe')
        sources=[root/'tools/tests/ebyte_pair_turnaround.cpp',peer/'src/nrf24.cpp',peer/'src/radio_manager.cpp']
        subprocess.run([args.compiler,*common,*peer_flags,'-I',str(peer/'include'),'-I',str(peer/'test/include'),*(str(p) for p in sources),'-o',str(binary)],env=env,check=True)
        result=subprocess.run([str(binary)],env=env,check=True,text=True,capture_output=True)
        print(label,result.stdout)
        (out/(label+'_turnaround.log')).write_text(result.stdout+result.stderr)
        results[label]={'output':result.stdout,'inputs':{str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in sources}}
    fingerprints=[re.search(r'WIRE .+',r['output']).group() for r in results.values()]
    assert len(set(fingerprints))==1
    (out/'verification.json').write_text(json.dumps(results,indent=2)+'\n')
    print('PASS both peer protocol settings and production reliable timing, receive preservation, failure handling and noACK policy')

if __name__=='__main__':main()
