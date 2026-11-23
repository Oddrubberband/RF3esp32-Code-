"""Run the production console parser using a buffered byte-stream adapter."""
import argparse
import os
from pathlib import Path
import subprocess

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--compiler',default=r'C:\msys64\ucrt64\bin\g++.exe')
    args=parser.parse_args()
    root=Path(__file__).resolve().parents[2]
    source=(root/'src/main.cpp').read_text()
    run=source[source.index('    void run()'):source.index('\nprivate:',source.index('    void run()'))]
    run=run.replace('std::fgetc(stdin)','readByte()').replace('clearerr(stdin);','').replace('std::printf','quietPrintf').replace('std::fflush(stdout);','')
    cpp=r"""
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include "host_transfer_protocol.hpp"
constexpr size_t kConsoleLineBytes=HostTransferProtocol::kConsoleLineBytes;
enum class CommandOrigin{Local};
std::string trimAscii(const std::string& value){auto first=value.find_first_not_of(" \t\r\n");if(first==std::string::npos)return {};auto last=value.find_last_not_of(" \t\r\n");return value.substr(first,last-first+1);}
struct End{};
constexpr unsigned pdMS_TO_TICKS(unsigned value){return value;}
void vTaskDelay(unsigned){throw End{};}
template<typename... Args> void quietPrintf(const char*,Args...){}
class App{
public:
    std::string input;size_t cursor=0;
    std::vector<std::string> commands;
    int readByte(){return cursor==input.size()?EOF:static_cast<unsigned char>(input[cursor++]);}
    void printPrompt(){}
    void dispatchCommand(const std::string& command,CommandOrigin){commands.push_back(command);}
"""+run+r"""
};
int main(){
    App app;
    const std::string upload="UPLOAD DATA 0 "+std::string(512,'A');
    app.input="HELLO\r\nSTATUS\nCW\tSTOP\rPOWER 31\b\n"+upload+"\r\n";
    try{app.run();}catch(const End&){}
    assert((app.commands==std::vector<std::string>{"HELLO","STATUS","CW STOP","POWER 3",upload}));
    App oversized;oversized.input=std::string(kConsoleLineBytes+100,'Z')+"\nSTATUS\n";
    try{oversized.run();}catch(const End&){}
    assert(oversized.commands==std::vector<std::string>{"STATUS"});
    std::puts("Production console long negotiated upload/CRLF/tab/backspace/line-capacity/resynchronization semantics PASS");
}
"""
    out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    path=out/'console_runtime.cpp';path.write_text(cpp)
    binary=out/'console_runtime.exe'
    env=os.environ.copy();env['PATH']=str(Path(args.compiler).parent)+os.pathsep+env['PATH']
    subprocess.run([args.compiler,'-std=c++17','-Wall','-Wextra','-Werror','-pedantic','-I',str(root/'include'),str(path),'-o',str(binary)],env=env,check=True)
    subprocess.run([str(binary)],env=env,check=True)
    # The production driver ring accepts several complete maximum-size commands.
    assert 'uart_driver_install(console_uart, 4096, 0, 0, nullptr, 0)' in source
    assert 'uart_vfs_dev_use_driver(console_uart)' in source
    print('UART 4096-byte interrupt-driver/VFS installation present in final compiled firmware')
if __name__=='__main__':main()
