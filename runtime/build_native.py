"""Build IMAPipe's closed set of native media components.

The private gst-full build has no filesystem plugin registry, dynamic plugin
scan, command-line pipeline parser, tracers, or GStreamer debug/key dumps.
External media I/O enters through the Runtime's appsrc/appsink/socket bridge.
"""
import argparse, hashlib, json, os, pathlib, subprocess, shutil, sys
p=argparse.ArgumentParser()
p.add_argument('--meson',required=True)
p.add_argument('--build',type=pathlib.Path,required=True)
p.add_argument('--prefix',type=pathlib.Path,required=True)
p.add_argument('--srtp-prefix',type=pathlib.Path,required=True)
p.add_argument('--dependency-prefix',type=pathlib.Path,action='append',default=[])
p.add_argument('--jobs',type=int,default=2)
p.add_argument('--configure-only',action='store_true')
a=p.parse_args();source=pathlib.Path(__file__).resolve().parents[1]
revision=subprocess.check_output(['git','-C',str(source),'rev-parse','HEAD'],text=True).strip()
if subprocess.check_output(['git','-C',str(source),'status','--porcelain'],text=True).strip():
 raise SystemExit('Build provenance requires a committed, clean GStreamer checkout')
native_lock=json.loads((source/'runtime/native-lock.json').read_text())
srtp_manifest=json.loads((a.srtp_prefix/'build.json').read_text())
if srtp_manifest['source_commit'] != native_lock['libsrtp']['revision']:
 raise SystemExit('The SRTP artifact does not match the pinned native state engine')
for filename,digest in srtp_manifest['artifacts'].items():
 if hashlib.sha256((a.srtp_prefix/filename).read_bytes()).hexdigest() != digest:
  raise SystemExit('The SRTP artifact changed after its build')
a.dependency_prefix.insert(0,a.srtp_prefix)
env=dict(os.environ)
if sys.platform == 'darwin':
 env['MACOSX_DEPLOYMENT_TARGET']='13.0'
env['PATH']=str(pathlib.Path(a.meson).resolve().parent)+os.pathsep+env.get('PATH','')
if a.dependency_prefix:
 env['PKG_CONFIG_PATH']=os.pathsep.join(str(path/'lib/pkgconfig') for path in a.dependency_prefix)
options=['-Dpkg_config_path='+','.join(str(path/'lib/pkgconfig') for path in a.dependency_prefix),'--buildtype=release','--default-library=static','-Dauto_features=disabled',
 '--force-fallback-for=glib,libffi,libpcre2-8,intl',
 '-Dglib:tests=false','-Dglib:nls=disabled','-Dglib:glib_debug=disabled',
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
if (a.build/'meson-private/coredata.dat').exists():command.extend(['--reconfigure','--clearcache'])
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
 (a.prefix/'build.json').write_text(json.dumps({'source_commit':revision,'source':str(source),'library':library['filename'],'configuration':command, 'native_lock':native_lock, 'srtp_prefix':str(a.srtp_prefix.resolve()), 'artifacts':{str(path.relative_to(a.prefix)):hashlib.sha256(path.read_bytes()).hexdigest() for path in destination.iterdir() if path.is_file()}},indent=2)+'\n')
