import re, zlib

cpp_path = r'C:\Users\jason\Documents\trae_projects\VJprg\vjvcplus_build\vjvcplus_autogen\include_Release\WORAJ6MAXX\qrc_viz_Release_CMAKE_.cpp'
out_path = r'C:\Users\jason\Documents\trae_projects\VJprg\viz_qml_restored.qml'

with open(cpp_path, 'r', encoding='utf8') as f:
    s = f.read()

m = re.search(r'static const unsigned char qt_resource_data\[\] = \{([\s\S]*?)\};', s)
hexes = re.findall(r'0x[0-9a-fA-F]{2}', m.group(1))
data = bytes(int(h, 16) for h in hexes)
print('total bytes:', len(data))

# Qt rcc 6 格式：每个条目 = 4字节LE压缩后长度 + 4字节LE未压缩长度 + zlib数据
# 但也可能是 BE。试前 20 个 offset + 多种 wbits
for off in range(20):
    for wbits in [15, -15]:
        try:
            out = zlib.decompress(data[off:], wbits)
            txt = out.decode('utf8', errors='replace')
            if 'import QtQuick' in txt:
                print('SUCCESS at offset', off, 'wbits', wbits, 'len', len(txt))
                with open(out_path, 'w', encoding='utf8') as wf:
                    wf.write(txt)
                print('SAVED')
                raise SystemExit(0)
        except Exception:
            pass

# 也试用 decompressobj 分块 flush 方式
for off in range(20):
    try:
        d = zlib.decompressobj()
        out = d.decompress(data[off:]) + d.flush()
        txt = out.decode('utf8', errors='replace')
        if 'import QtQuick' in txt:
            print('SUCCESS2 at offset', off, 'len', len(txt))
            with open(out_path, 'w', encoding='utf8') as wf:
                wf.write(txt)
            print('SAVED')
            raise SystemExit(0)
    except Exception:
        pass

print('ALL FAILED')
