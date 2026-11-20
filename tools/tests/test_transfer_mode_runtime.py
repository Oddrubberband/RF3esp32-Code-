"""Exercise production loop preparation, mode telemetry and antenna command guards."""
import argparse
import importlib.util
import os
from pathlib import Path
import subprocess

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--compiler',default=r'C:\msys64\ucrt64\bin\g++.exe')
    args=parser.parse_args()
    root=Path(__file__).resolve().parents[2]
    spec=importlib.util.spec_from_file_location('runtime',root/'tools/tests/test_host_runtime_contract.py')
    module=importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    cpp=module.harness(root)
    cpp=cpp[:cpp.index('int main(int argc,char** argv)')]
    source=(root/'src/main.cpp').read_text()
    start=source.index('    void runTxLoopIteration(')
    end=source.index('        const bool cutoff',start)
    prep=source[start:end].replace('runTxLoopIteration','prepareTxLoopIteration').replace('takeRadio()','takeRadio(pdMS_TO_TICKS(50))')+'        giveRadio();\n    }\n'
    start=source.index('    bool commandFem(')
    end=source.index('    bool commandPower(',start)
    command=source[start:end]
    insertion=cpp.rfind('};')
    cpp=cpp[:insertion]+'bool incoming=false;bool hasActiveIncomingReceiveLocked(){return incoming;}\n'+prep+command+cpp[insertion:]
    cpp=cpp.replace('unsigned femAntenna()const{return 0;}bool selectFemAntenna(unsigned){return false;}','unsigned antenna=0;unsigned femAntenna()const{return antenna;}bool selectFemAntenna(unsigned value){antenna=value;return true;}')
    cpp+=r"""
int main(int argc,char** argv){
    assert(argc==2);file_root=argv[1];std::filesystem::create_directories(file_root);
    DemoConsoleApp app;app.manager_.value.state=RadioState::Standby;
    LoopConfig loop;loop.file_name="file.bin";
    for(bool no_ack:{false,true}){
        loop.no_ack=no_ack;
        app.last_tx_report_={};
        app.prepareTxLoopIteration(loop);
        assert(app.last_tx_report_.no_ack==no_ack);
        const auto id=app.last_tx_report_.transfer_id;
        app.last_tx_report_.state=ReliableTransferV2::SenderState::Cancelled;
        app.prepareTxLoopIteration(loop);assert(app.last_tx_report_.no_ack==no_ack && app.last_tx_report_.transfer_id!=id);
        auto terminal=copyTransferReport(app.last_tx_report_);
        terminal.state=ReliableTransferV2::SenderState::Cancelled;terminal.error=ProtocolV2::ErrorCode::Cancelled;
        terminal.bytes_transferred=20;terminal.total_bytes=40;
        publishTransferReport(app.last_tx_report_,terminal);
        HttpStatusSnapshot status;assert(app.captureHttpStatus(status));
        assert(status.tx.no_ack==no_ack && status.tx.id==terminal.transfer_id && status.tx.bytes==20);
        app.printStatus();
        std::puts(app.buildStatusJson().c_str());
    }
    assert(!app.commandFem({"FEM","ANT","2"},CommandOrigin::Local));
    app.hal_.antenna=1;assert(app.commandFem({"FEM","ANT","2"},CommandOrigin::Local));assert(app.hal_.antenna==2);
    assert(!app.commandFem({"FEM","ANT","1"},CommandOrigin::Remote));
    app.loop_config_.active=true;assert(!app.commandFem({"FEM","ANT","1"},CommandOrigin::Local));
    app.loop_config_.active=false;app.manager_.value.state=RadioState::RxListening;assert(!app.commandFem({"FEM","ANT","1"},CommandOrigin::Local));
    app.manager_.value.state=RadioState::Standby;app.incoming=true;assert(!app.commandFem({"FEM","ANT","1"},CommandOrigin::Local));
    app.incoming=false;app.radio_available=false;assert(!app.commandFem({"FEM","ANT","1"},CommandOrigin::Local));
    assert(!app.loop_held && !app.radio_held);app.radio_available=true;
    assert(app.commandFem({"FEM","ANT","1"},CommandOrigin::Local));
    assert(app.buildStatusJson().find("\"fem_antenna\":1")!=std::string::npos);
    std::puts("Production loop preparation/terminal mode parity, antenna origin/idle/RX/lock guards PASS");
}
"""
    out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    path=out/'mode_runtime.cpp';path.write_text(cpp)
    binary=out/'mode_runtime.exe'
    flags=['-DRF3_HARDWARE_PROFILE_ID=1','-DRF3_BLUETOOTH_CONTROL_ENABLED=1','-DNRF24_CE_PIN=17','-DNRF24_CSN_PIN=27','-DNRF24_IRQ_PIN=16','-DNRF24_SCK_PIN=18','-DNRF24_MOSI_PIN=23','-DNRF24_MISO_PIN=19']
    env=os.environ.copy();env['PATH']=str(Path(args.compiler).parent)+os.pathsep+env['PATH']
    subprocess.run([args.compiler,'-std=c++17','-Wall','-Wextra','-Werror','-pedantic','-pthread',*flags,'-I',str(root/'include'),str(path),'-o',str(binary)],env=env,check=True)
    result=subprocess.run([str(binary),str(out/'files')],env=env,check=True,capture_output=True,text=True)
    (out/'mode_runtime.log').write_text(result.stdout)
    lines=result.stdout.splitlines()
    human=[line for line in lines if line.startswith('profile=')]
    assert len(human)==2 and 'tx_no_ack=false' in human[0] and 'tx_no_ack=true' in human[1]
    print(lines[-1])
if __name__=='__main__':main()
