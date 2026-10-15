"""Build the stop regression and its Rust-only fixture with the private artifact."""
import argparse
import os
import pathlib
import shlex
import subprocess

p = argparse.ArgumentParser()
p.add_argument('--build', type=pathlib.Path, required=True)
p.add_argument('--output', type=pathlib.Path, required=True)
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=True)
root = pathlib.Path(__file__).resolve().parents[2]
env = dict(os.environ, PKG_CONFIG_PATH=str(a.build / 'meson-uninstalled'))
flags = shlex.split(subprocess.check_output(
    ['pkg-config', '--cflags', '--libs', 'gstreamer-full-1.0', 'gio-2.0', 'gmodule-2.0'],
    env=env, text=True))
common = ['cc', '-std=c11', '-g', '-O1', '-fvisibility=hidden', '-ffunction-sections',
          '-Wl,-dead_strip', str(root / 'runtime/tests/rtp_stop.c'), *flags,
          '-Wl,-rpath,' + str(a.build.resolve())]
subprocess.run([*common, '-o', str(a.output / 'rtp_stop')], check=True)
subprocess.run([*common, '-shared', '-DRTP_STOP_FIXTURE', '-o',
                str(a.output / 'librtp_stop_fixture.dylib')], check=True)
subprocess.run([str(a.output / 'rtp_stop')], check=True)
