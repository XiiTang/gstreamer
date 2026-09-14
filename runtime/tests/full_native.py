"""Check the actual private gst-full artifact; no system GStreamer plugins."""
import argparse,os,pathlib,shlex,subprocess
p=argparse.ArgumentParser();p.add_argument('--build',type=pathlib.Path,required=True);p.add_argument('--output',type=pathlib.Path,required=True)
a=p.parse_args();root=pathlib.Path(__file__).resolve().parents[2];a.output.mkdir(parents=True,exist_ok=True)
env=dict(os.environ,PKG_CONFIG_PATH=str(a.build/'meson-uninstalled'))
def flags(*args):return shlex.split(subprocess.check_output(['pkg-config',*args,'gstreamer-full-1.0','gio-2.0','openssl'],env=env,text=True))
for name in ['rtsp_transport','payload','rtp_session','dtls_owner','rtp_feedback','sdp_selection']:
 subprocess.run(['cc','-std=c11','-g','-I'+str(root/'runtime'),'-I'+str(root/'subprojects/gst-plugins-bad/ext/dtls'),'-I'+str(root/'subprojects/gst-plugins-base/gst-libs/gst/rtsp'),*flags('--cflags'),str(root/'runtime/tests'/f'{name}.c'),*flags('--libs'),'-Wl,-rpath,'+str(a.build.resolve()),'-o',str(a.output/name)],check=True)
registry=a.output/'must-not-create-registry.bin'
env.update(GST_PLUGIN_PATH_1_0='/nonexistent/forbidden',GST_PLUGIN_SYSTEM_PATH_1_0='/nonexistent/forbidden',GST_REGISTRY_1_0=str(registry),GST_DEBUG='9')
subprocess.run([str(a.output/'sdp_selection')],env=env,check=True)
subprocess.run([str(a.output/'rtp_feedback')],env=env,check=True)
subprocess.run([str(a.output/'dtls_owner')],env=env,check=True)
subprocess.run([str(a.output/'rtsp_transport')],env=env,check=True)
subprocess.run([str(a.output/'rtp_session')],env=env,check=True)
subprocess.run([str(a.output/'rtp_session'),'periodic'],env=env,check=True)
common=['ffmpeg','-v','error','-y','-f','lavfi','-i','color=c=black:s=64x64:r=1','-frames:v','1','-an']
subprocess.run([*common,'-c:v','libx264','-preset','ultrafast','-tune','zerolatency','-f','h264',str(a.output/'h264')],check=True)
subprocess.run([*common,'-c:v','libx265','-x265-params','pools=1:frame-threads=1:log-level=error','-f','hevc',str(a.output/'h265')],check=True)
subprocess.run([*common,'-c:v','mjpeg','-huffman','default','-pix_fmt','yuvj420p','-f','mjpeg',str(a.output/'jpeg')],check=True)
subprocess.run([*common,'-c:v','mjpeg','-huffman','optimal','-pix_fmt','yuvj420p','-f','mjpeg',str(a.output/'jpeg-optimized')],check=True)
subprocess.run(['ffmpeg','-v','error','-y','-f','lavfi','-i','anullsrc=r=44100:cl=stereo','-frames:a','1','-c:a','aac','-f','adts',str(a.output/'aac.adts')],check=True)
adts=(a.output/'aac.adts').read_bytes();size=((adts[3]&3)<<11)|(adts[4]<<3)|(adts[5]>>5);header=7 if adts[1]&1 else 9
(a.output/'aac').write_bytes(adts[header:size])
subprocess.run([str(a.output/'payload'),str(a.output)],env=env,check=True)
for name,fmt in [('h264','h264'),('h265','hevc'),('jpeg','mjpeg')]:
 def pixels(path):return subprocess.check_output(['ffmpeg','-v','error','-f',fmt,'-i',str(path),'-f','rawvideo','-pix_fmt','yuv420p','-'])
 assert pixels(a.output/name)==pixels(a.output/(name+'.recovered')),name
 assert pixels(a.output/name)==pixels(a.output/(name+'.recovered.session')),name
 print('PASS independent decoded pixels for standalone and shared session:',name)
assert not registry.exists(),'Private media build read/wrote a filesystem registry'
