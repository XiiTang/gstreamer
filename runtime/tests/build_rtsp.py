"""Build RTSP's controlled native transport against explicit test dependencies."""
import argparse, os, pathlib, shlex, subprocess
p=argparse.ArgumentParser()
p.add_argument('--prefix',type=pathlib.Path,required=True)
p.add_argument('--config',type=pathlib.Path,required=True)
p.add_argument('--output',type=pathlib.Path,required=True)
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
root=pathlib.Path(__file__).resolve().parents[2]
source=root/'subprojects/gst-plugins-base/gst-libs/gst/rtsp'
env=dict(os.environ,PKG_CONFIG_PATH=str(a.prefix/'lib/pkgconfig'))
# Link the selected core/base/GIO only. Compile this fork's entire RTSP library.
def flags(*args):
 return shlex.split(subprocess.check_output(['pkg-config',*args,'gstreamer-base-1.0','gio-2.0'],env=env,text=True))
common=['cc','-std=c11','-DHAVE_CONFIG_H','-DBUILDING_GST_RTSP','-DG_LOG_DOMAIN="GStreamer-RTSP"','-fPIC','-g','-O1','-I'+str(source),'-I'+str(source.parents[1]),'-I'+str(a.config),'-I'+str(a.config/'gst-libs/gst/rtsp'),*flags('--cflags')]
files=[str(source/(n+'.c')) for n in ['gstrtsptransport','gstrtspurl','gstrtspmessage','gstrtspconnection','gstrtspruntimeclient','gstrtspdefs','gstrtspextension','gstrtsprange']]
files.append(str(a.config/'gst-libs/gst/rtsp/gstrtsp-enumtypes.c'))
subprocess.run([*common,str(root/'runtime/tests/rtsp_transport.c'),*files,*flags('--libs'),'-o',str(a.output/'rtsp_transport')],check=True)
