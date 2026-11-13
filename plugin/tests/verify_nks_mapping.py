"""Check exported HS2 NKS mapping, embedded VST3 state and optional previews (requires msgpack)."""
import argparse
from pathlib import Path
import struct
import re
import subprocess
import xml.etree.ElementTree as ET

import msgpack


def chunks(data):
    assert data[:4] == b'RIFF' and data[8:12] == b'NIKS'
    start = 12
    while start + 8 <= len(data):
        name, size = struct.unpack_from('<4sI', data, start)
        end = start + 8 + size
        assert end <= len(data)
        yield name, data[start + 8:end]
        start = end + (size & 1)
    assert start == len(data)


def verify(nks_root, vst_root, ffmpeg=None):
    counts = {'Patches': 0, 'Combis': 0}
    page_counts = {'Patches': set(), 'Combis': set()}
    bank_programs = {}
    previews = set()
    audio_samples = {}
    for path in nks_root.rglob('*.nksf'):
        relative = path.relative_to(nks_root)
        family = relative.parts[0]
        fields = dict(chunks(path.read_bytes()))
        info = msgpack.unpackb(fields[b'NISI'][4:], raw=False)
        bank_number, _, title = relative.parts[1].partition(' ')
        label = f'{int(bank_number):02d} {title}'
        if int(bank_number) >= 100:
            label = 'Bank ' + label
        assert info['bankchain'] == ['HS2 Port', label, ''], path
        state = fields[b'PCHK'][4:]
        xml = re.search(rb'<HS2Port\b.*?</HS2Port>', state, re.DOTALL)
        assert xml, path
        setup = ET.fromstring(xml[0]).find('SETUP')
        part = setup.find('PART')
        index = int(setup.attrib['combiIndex'] if family == 'Combis' else part.attrib['patch'])
        name = setup.attrib['combiName'] if family == 'Combis' else part.attrib['name']
        prefix, _, display_name = info['name'].partition(' ')
        assert prefix.isdecimal() and int(prefix) == index + 1 and display_name == name, path
        bank_programs.setdefault(bank_number, []).append((int(prefix), len(prefix)))
        pages = msgpack.unpackb(fields[b'NICA'][4:], raw=False)['ni8']
        assert pages and all(0 < len(page) <= 8 and page[0].get('section') for page in pages), path
        ids = [control['id'] for page in pages for control in page]
        assert len(ids) == len(set(ids)) and all(0 <= index < 864 for index in ids), path
        assert all(control['name'].strip() for page in pages for control in page), path
        assert 0 in ids and len(pages) > 2, path
        assert fields[b'PCHK'][4:] == (vst_root / relative.with_suffix('.vstpreset')).read_bytes(), path
        if ffmpeg:
            preview = path.parent / '.previews' / (path.name + '.ogg')
            assert preview.is_file(), preview
            with preview.open('rb') as audio:
                assert audio.read(4) == b'OggS', preview
            previews.add(preview)
            audio_samples.setdefault(family, preview)
        if family == 'Patches':
            sections = {page[0]['section'] for page in pages}
            assert {'Mixer', 'Voice', 'Key and Velocity', 'Tuning', 'Arpeggiator', 'Arp and Part'} <= sections, path
        counts[family] += 1
        page_counts[family].add(len(pages))
    assert all(counts.values()), counts
    for programs in bank_programs.values():
        width = max(2, len(str(max(number for number, _ in programs))))
        assert all(digits == width for _, digits in programs), programs
    for family, count in counts.items():
        print(f'{family}: {count} checked; {min(page_counts[family])}-{max(page_counts[family])} pages')
    if ffmpeg:
        assert previews == set(nks_root.rglob('*.nksf.ogg'))
        for family, preview in audio_samples.items():
            run = subprocess.run(
                [ffmpeg, '-hide_banner', '-nostdin', '-i', str(preview),
                 '-af', 'volumedetect', '-f', 'null', '-'],
                check=True, capture_output=True, text=True, timeout=20,
            )
            assert '44100 Hz, stereo' in run.stderr, preview
            peak = re.search(r'max_volume: ([-\d.]+) dB', run.stderr)
            assert peak and float(peak[1]) > -70, preview
            print(f'{family}: preview audio decodes, peak {peak[1]} dBFS')
        print(f'{len(previews)} previews matched their preset filenames')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('nks_root', type=Path)
    parser.add_argument('vst_root', type=Path)
    parser.add_argument('--ffmpeg', help='Require one Ogg preview per preset and decode a sample from each family')
    arguments = parser.parse_args()
    verify(arguments.nks_root, arguments.vst_root, arguments.ffmpeg)
