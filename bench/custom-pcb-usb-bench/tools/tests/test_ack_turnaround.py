"""Run the real driver/manager turnaround trace with the custom release flags."""
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
    out=args.output.resolve()
    out.mkdir(parents=True,exist_ok=True)
    binary=out/'ack_turnaround.exe'
    env=os.environ.copy()
    env['PATH']=str(Path(args.compiler).parent)+os.pathsep+env['PATH']
    subprocess.run([
        args.compiler,'-std=c++17','-Wall','-Wextra','-Werror','-pedantic',
        '-DRF3_AUTOMATIC_FEM=1','-DRF3_NRF24_TX_CE_PULSE_US=40',
        '-I',str(root/'include'),'-I',str(root/'test/include'),
        str(root/'tools/tests/ack_turnaround.cpp'),
        str(root/'src/nrf24.cpp'),str(root/'src/radio_manager.cpp'),
        '-o',str(binary),
    ],env=env,check=True)
    result=subprocess.run([str(binary)],env=env,check=True,capture_output=True,text=True)
    (out/'ack-turnaround.log').write_text(result.stdout+result.stderr,encoding='utf-8')
    print(result.stdout,end='')
if __name__=='__main__':main()
