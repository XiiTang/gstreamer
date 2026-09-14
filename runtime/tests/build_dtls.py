"""Build the patched DTLS module against an explicitly selected native prefix."""
import argparse, os, pathlib, shlex, subprocess
p=argparse.ArgumentParser()
p.add_argument('--prefix',type=pathlib.Path,required=True)
p.add_argument('--config',type=pathlib.Path,required=True)
p.add_argument('--openssl-prefix',type=pathlib.Path,required=True)
p.add_argument('--output',type=pathlib.Path,required=True)
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
root=pathlib.Path(__file__).resolve().parents[2]
source=root/'subprojects/gst-plugins-bad/ext/dtls'
env=dict(os.environ,PKG_CONFIG_PATH=os.pathsep.join(str(p/'lib/pkgconfig') for p in [a.prefix,a.openssl_prefix]))
def flags(*args):
 return shlex.split(subprocess.check_output(['pkg-config',*args,'gstreamer-1.0','openssl'],env=env,text=True))
common=['cc','-std=c11','-DHAVE_CONFIG_H','-fPIC','-g','-O1','-I'+str(a.config),'-I'+str(source),*flags('--cflags')]
libs=flags('--libs')
files=[str(source/(n+'.c')) for n in ['gstdtlsagent','gstdtlscertificate','gstdtlsconnection','gstdtlsdec','gstdtlsenc','gstdtlssrtpbin','gstdtlssrtpdec','gstdtlssrtpdemux','gstdtlssrtpenc','plugin','gstdtlselement']]
subprocess.run([*common,'-shared',*files,*libs,'-o',str(a.output/'libgstdtls.dylib')],check=True)
if (root/'runtime/tests/dtls_interop.c').exists():
 subprocess.run([*common,str(root/'runtime/tests/dtls_interop.c'),*files[:3],*libs,'-o',str(a.output/'dtls_interop')],check=True)
