"""Build IMAPipe's closed set of native media components.

The private gst-full build has no filesystem plugin registry, dynamic plugin
scan, command-line pipeline parser, tracers, or GStreamer debug/key dumps.
External media I/O enters through the Runtime's appsrc/appsink/socket bridge.
"""
import argparse, json, os, pathlib, subprocess, shutil
p=argparse.ArgumentParser()
p.add_argument('--meson',required=True)
p.add_argument('--build',type=pathlib.Path,required=True)
p.add_argument('--prefix',type=pathlib.Path,required=True)
p.add_argument('--dependency-prefix',type=pathlib.Path,action='append',default=[])
p.add_argument('--jobs',type=int,default=2)
p.add_argument('--configure-only',action='store_true')
a=p.parse_args();source=pathlib.Path(__file__).resolve().parents[1]
env=dict(os.environ)
env['PATH']=str(pathlib.Path(a.meson).resolve().parent)+os.pathsep+env.get('PATH','')
if a.dependency_prefix:
 env['PKG_CONFIG_PATH']=os.pathsep.join(str(path/'lib/pkgconfig') for path in a.dependency_prefix)
options=['--buildtype=release','--default-library=static','-Dauto_features=disabled',
 '-Dbase=enabled','-Dgood=enabled','-Dbad=enabled','-Dugly=disabled','-Dlibav=disabled',
 '-Ddevtools=disabled','-Dges=disabled','-Drtsp_server=disabled','-Dorc=disabled',
 '-Dintrospection=disabled','-Dtests=disabled','-Dexamples=disabled','-Ddoc=disabled',
 '-Dgstreamer:registry=false','-Dgstreamer:gst_debug=false','-Dgstreamer:gst_parse=false',
 '-Dgstreamer:tracer_hooks=false','-Dgstreamer:tools=disabled',
 '-Dgst-plugins-base:app=enabled','-Dgst-plugins-good:rtp=enabled',
 '-Dgst-plugins-good:rtpmanager=enabled','-Dgst-plugins-bad:srtp=enabled',
 '-Dgst-plugins-bad:dtls=enabled','-Dgst-full=enabled',
 '-Dgst-full-libraries=gstreamer-app-1.0,gstreamer-rtp-1.0,gstreamer-rtsp-1.0,gstreamer-sdp-1.0','-Dgst-full-plugins='+(';'.join((('gst'+name+'.lib') if os.name=='nt' else ('libgst'+name+'.a')) for name in ['app','rtp','rtpmanager','srtp','dtls']))]
command=[a.meson,'setup',str(a.build.resolve()),str(source),'--prefix='+str(a.prefix.resolve()),*options]
if (a.build/'meson-private/coredata.dat').exists():command.append('--reconfigure')
a.build.mkdir(parents=True,exist_ok=True)
(a.build/'imapipe-configuration.json').write_text(json.dumps(command,indent=2)+'\n')
subprocess.run(command,env=env,check=True)
if not a.configure_only:
 subprocess.run([a.meson,'compile','-C',str(a.build),'-j',str(a.jobs),'gstreamer-full-1.0'],env=env,check=True)
 targets=json.loads(subprocess.check_output([a.meson,'introspect',str(a.build),'--targets'],env=env,text=True))
 library=next(target for target in targets if target['name']=='gstreamer-full-1.0')
 destination=a.prefix/'lib';destination.mkdir(parents=True,exist_ok=True)
 for filename in library['filename']:
  artifact=pathlib.Path(filename)
  shutil.copy2(artifact,destination/artifact.name)
 (a.prefix/'build.json').write_text(json.dumps({'source':str(source),'library':library['filename'],'configuration':command},indent=2)+'\n')
