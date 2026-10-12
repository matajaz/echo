#!/usr/bin/env python3
"""Build/render real LVGL page code on the host. No device/network needed."""
from pathlib import Path
import argparse
import subprocess
import re
ROOT=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser()
parser.add_argument('--output',type=Path,default=Path('/private/tmp/echo-ui-preview'))
args=parser.parse_args()
out=args.output.resolve(); out.mkdir(parents=True,exist_ok=True)
lvgl=ROOT/'managed_components/lvgl__lvgl'
host=ROOT/'tools/ui_host'
build=out/'lvgl-build'
version=re.search(r'set\(PROJECT_VER\s+"([^"]+)"\)',(ROOT/'CMakeLists.txt').read_text()).group(1)
with (out/'build.log').open('w') as log:
    subprocess.run(['cmake','-S',str(lvgl),'-B',str(build),'-DLV_CONF_PATH='+str(host/'lv_conf.h'),'-DCMAKE_BUILD_TYPE=Release'],check=True,stdout=log,stderr=log)
    subprocess.run(['cmake','--build',str(build),'--target','lvgl','-j','4'],check=True,stdout=log,stderr=log)
    cjson=ROOT/'managed_components/espressif__cjson/cJSON'
    sources=[ROOT/'main/echo_sun_20.c',host/'preview.c',ROOT/'main/ui/ui_widgets.c',ROOT/'main/ui_graphs.c',ROOT/'core/ha_parse.c',cjson/'cJSON.c',*sorted((ROOT/'main/ui/pages').glob('*.c'))]
    subprocess.run(['cc','-std=gnu11','-O1','-g','-Wall','-Wextra','-Wno-deprecated-declarations','-DLV_CONF_INCLUDE_SIMPLE',
        f'-DECHO_PREVIEW_VERSION="{version}"',
        *[a for p in [host,ROOT/'main',ROOT/'main/ui',ROOT/'core',lvgl,cjson] for a in ['-I',str(p)]],
        *map(str,sources),str(build/'lib/liblvgl.a'),'-lm','-o',str(out/'preview')],check=True,stdout=log,stderr=log)
result=subprocess.run([str(out/'preview'),str(ROOT/'core/test/fixtures/nordpool.json'),str(out)])
from PIL import Image,ImageDraw
images=[]
for i in range(9):
    image=Image.open(out/f'page-{i}.ppm'); image.save(out/f'page-{i}.png'); images.append(image)
gallery=Image.new('RGB',(3*256,3*344),'#080d14')
for i,image in enumerate(images): gallery.paste(image,((i%3)*256+8,(i//3)*344+8))
gallery.save(out/'pages.png')
print('Rendered page gallery:',out/'pages.png')
raise SystemExit(result.returncode)
