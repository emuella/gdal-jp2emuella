#!/usr/bin/env python3
"""Compare the locked small independent NITF pack through indexed JP2Emuella.

Uses Python 3.11+ standard-library modules and the fixture owner's scalar
oracle at runtime. Binary registration is explicit; this is not an autoload
test. All source and reference pixels remain in memory.
"""

import argparse
from array import array
import ctypes as C
import hashlib
import importlib.util
import json
from pathlib import Path
import sys


TESTDATA_REVISION = '2d519ddaf019f10b9e409ea3338d395438486647'
OWNER_FILES = {
    'recipes/check-independent-nitf.py':
        '0cf2199da002d7a6c9098a2007e4ee499b7e24e122b2935367890eba931de5ab',
    'recipes/independent-nitf-v1.py':
        '90c3b17d60581a11e516cdd083cba9f7ccc594f62e5ac31dbb73eb176c3511da',
    'manifests/jpeg-2000/independent-nitf.toml':
        'e8f9a57a98d7211f89a18798180794cf1803e91387e9c350a04ba4c9532bc18f',
}
PROVENANCE_SHA256 = '993820dccf80e8a7259eb9c7935405419730aebc55ac43b5c662c07564aeef51'


def require(condition, message):
    if not condition:
        raise ValueError(message)


def identity(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return {'path': str(path.resolve()), 'bytes': path.stat().st_size,
            'sha256': digest.hexdigest()}


def load_owner(source, pack):
    # Pin executable oracle/parser content before importing it. The checkout
    # may be newer, but these consumed contracts must match the recorded owner.
    require(__debug__, 'Run without -O: the owner checker uses assertions')
    for name, digest in OWNER_FILES.items():
        require(identity(source / name)['sha256'] == digest,
                f'Fixture-owner contract differs from {TESTDATA_REVISION}: {name}')
    provenance_path = pack / 'PROVENANCE.json'
    require(identity(provenance_path)['sha256'] == PROVENANCE_SHA256,
            'Input pack is not the locked small independent NITF pack')
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location(
        'independent_nitf_owner', source / 'recipes/check-independent-nitf.py')
    owner = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(owner)
    owner.ROOT, owner.PACK = source, pack
    # Includes the complete asset inventory, hashes, NITF segment identity,
    # genuine SIZ/COD parameters and scalar source-pixel digest for every case.
    owner.check()
    return owner, json.loads(provenance_path.read_text())


def pixel_digest(case, sample):
    """Normalise samples to the fixture owner's tile/interleave/byte order."""
    digest = hashlib.sha256()
    size = 1 if case['bits'] == 8 else 2
    for top in range(0, case['height'], 1024):
        for left in range(0, case['width'], 1024):
            for y in range(top, min(top + 1024, case['height'])):
                for x in range(left, min(left + 1024, case['width'])):
                    for band in range(case['bands']):
                        digest.update(sample(x, y, band).to_bytes(size, 'little'))
    return digest.hexdigest()


class Gdal:
    """Explicitly typed public GDAL and plugin registration boundaries."""

    def __init__(self, gdal, codec, plugin):
        self.lib = C.CDLL(str(gdal), mode=C.RTLD_GLOBAL)
        self.codec = C.CDLL(str(codec), mode=C.RTLD_GLOBAL)
        self.plugin = C.CDLL(str(plugin))
        for symbol in ('emuella_j2k_decoder_create_indexed',
                       'emuella_j2k_decode_components_region'):
            require(C.cast(getattr(self.plugin, symbol), C.c_void_p).value ==
                    C.cast(getattr(self.codec, symbol), C.c_void_p).value,
                    f'Plugin resolves a different codec library for {symbol}')
        require(C.cast(self.plugin.GDALGetDriverByName, C.c_void_p).value ==
                C.cast(self.lib.GDALGetDriverByName, C.c_void_p).value,
                'Plugin resolves a different GDAL library')
        signatures = {
            'GDALRegister_NITF': (None, []),
            'GDALVersionInfo': (C.c_char_p, [C.c_char_p]),
            'CPLSetConfigOption': (None, [C.c_char_p, C.c_char_p]),
            'CPLGetLastErrorMsg': (C.c_char_p, []),
            'GDALGetDriverByName': (C.c_void_p, [C.c_char_p]),
            'GDALGetDriverCount': (C.c_int, []),
            'GDALGetDriver': (C.c_void_p, [C.c_int]),
            'GDALGetDriverShortName': (C.c_char_p, [C.c_void_p]),
            'GDALGetDatasetDriver': (C.c_void_p, [C.c_void_p]),
            'GDALOpenEx': (C.c_void_p, [C.c_char_p, C.c_uint,
                C.POINTER(C.c_char_p), C.POINTER(C.c_char_p), C.POINTER(C.c_char_p)]),
            'GDALClose': (C.c_int, [C.c_void_p]),
            'GDALGetRasterXSize': (C.c_int, [C.c_void_p]),
            'GDALGetRasterYSize': (C.c_int, [C.c_void_p]),
            'GDALGetRasterCount': (C.c_int, [C.c_void_p]),
            'GDALGetRasterBand': (C.c_void_p, [C.c_void_p, C.c_int]),
            'GDALGetRasterDataType': (C.c_int, [C.c_void_p]),
            'GDALGetMetadataItem': (C.c_char_p, [C.c_void_p, C.c_char_p, C.c_char_p]),
            'GDALRasterIO': (C.c_int, [C.c_void_p, C.c_int, C.c_int, C.c_int,
                C.c_int, C.c_int, C.c_void_p, C.c_int, C.c_int, C.c_int,
                C.c_int, C.c_int]),
        }
        for name, (result, arguments) in signatures.items():
            function = getattr(self.lib, name)
            function.restype, function.argtypes = result, arguments
        for name, value in [('GDAL_PAM_ENABLED', 'NO'), ('GDAL_DRIVER_PATH', 'disable'),
                            ('GDAL_NUM_THREADS', '1'),
                            ('JP2EMUELLA_REQUIRE_SOURCE_INDEX', 'YES')]:
            self.lib.CPLSetConfigOption(name.encode(), value.encode())
        self.lib.GDALRegister_NITF()
        register = self.plugin.GDALRegister_JP2Emuella
        register.restype, register.argtypes = None, []
        register()
        self.check_drivers()
        driver = self.lib.GDALGetDriverByName(b'JP2Emuella')
        require(self.metadata(driver, 'JP2EMUELLA_SOURCE_INDEX') == 'REQUIRED_SUPPORTED',
                'Plugin does not support required source indexing')

    def check_drivers(self):
        names = {self.lib.GDALGetDriverShortName(self.lib.GDALGetDriver(i)).decode()
                 for i in range(self.lib.GDALGetDriverCount())}
        require(names == {'NITF', 'JP2Emuella'},
                f'Unexpected drivers could affect decoder attribution: {sorted(names)}')

    def checked(self, value):
        if not value:
            raise RuntimeError(self.lib.CPLGetLastErrorMsg().decode())
        return value

    def metadata(self, handle, key, domain=''):
        value = self.lib.GDALGetMetadataItem(handle, key.encode(), domain.encode())
        return value.decode() if value is not None else None

    def open(self, name, driver, diagnostics=False):
        allowed = (C.c_char_p * 2)(driver.encode(), None)
        options = (C.c_char_p * 2)(b'DIAGNOSTICS=YES', None) if diagnostics else None
        dataset = self.checked(self.lib.GDALOpenEx(
            str(name).encode(), 2, allowed, options, None))  # Raster, read-only.
        actual = self.lib.GDALGetDriverShortName(self.lib.GDALGetDatasetDriver(dataset))
        if actual != driver.encode():
            self.lib.GDALClose(dataset)
            raise ValueError(f'Expected {driver}, got {actual!r}')
        return dataset

    def close(self, dataset):
        require(self.lib.GDALClose(dataset) == 0, 'GDAL dataset close failed')

    def read_case(self, path, case):
        outer = self.open(path, 'NITF')
        nested = None
        try:
            self.check_drivers()
            require((self.lib.GDALGetRasterXSize(outer), self.lib.GDALGetRasterYSize(outer),
                     self.lib.GDALGetRasterCount(outer)) ==
                    (case['width'], case['height'], case['bands']), 'NITF geometry mismatch')
            for key, value in case['nitf_metadata'].items():
                require(self.metadata(outer, key) == value, f'NITF metadata differs: {key}')
            nested_name = self.metadata(outer, 'JPEG2000_DATASET_NAME', 'DEBUG')
            require(nested_name is not None, 'NITF did not expose its embedded source')
            nested = self.open(nested_name, 'JP2Emuella', diagnostics=True)
            mode = self.metadata(nested, 'SOURCE_INDEX_REQUIRED', 'EMUELLA_DIAGNOSTICS')
            require(mode == '1', 'Embedded dataset did not select required source indexing')
            bits, planes, precision = case['bits'], [], []
            for band in range(case['bands']):
                outer_band = self.checked(self.lib.GDALGetRasterBand(outer, band + 1))
                nested_band = self.checked(self.lib.GDALGetRasterBand(nested, band + 1))
                dtype = 1 if bits == 8 else 2
                require(self.lib.GDALGetRasterDataType(outer_band) == dtype and
                        self.lib.GDALGetRasterDataType(nested_band) == dtype,
                        'Native sample type differs from genuine codestream precision')
                nbits = self.metadata(nested_band, 'NBITS', 'IMAGE_STRUCTURE')
                require(nbits == str(bits) or (bits == 8 and nbits is None),
                        'Embedded NBITS differs from locked SIZ precision')
                precision.append(nbits)
                samples = array('B' if bits == 8 else 'H', [0]) * (
                    case['width'] * case['height'])
                require(samples.itemsize == (1 if bits == 8 else 2), 'Unexpected native width')
                buffer = (C.c_ubyte * (len(samples) * samples.itemsize)).from_buffer(samples)
                status = self.lib.GDALRasterIO(
                    outer_band, 0, 0, 0, case['width'], case['height'], buffer,
                    case['width'], case['height'], dtype, samples.itemsize,
                    samples.itemsize * case['width'])
                self.checked(status == 0)
                planes.append(samples)
            return planes, {'outer_driver': 'NITF', 'nested_driver': 'JP2Emuella',
                            'nitf_ic': self.metadata(outer, 'NITF_IC'),
                            'nitf_abpp': self.metadata(outer, 'NITF_ABPP'),
                            'nested_nbits': precision, 'source_index_required': True}
        finally:
            if nested is not None:
                self.close(nested)
            self.close(outer)


def compare(case, planes, oracle, reference):
    source_peak = source_mismatches = source_squared = 0
    reference_peak = reference_mismatches = reference_squared = 0
    for y in range(case['height']):
        for x in range(case['width']):
            for band in range(case['bands']):
                index = y * case['width'] + x
                actual = planes[band][index]
                source = oracle(x, y, band, case['bits'], case['bands'])
                expected = reference[index] if case['lossy'] else source
                delta, source_delta = actual - expected, actual - source
                reference_peak = max(reference_peak, abs(delta))
                reference_mismatches += delta != 0
                reference_squared += delta * delta
                source_peak = max(source_peak, abs(source_delta))
                source_mismatches += source_delta != 0
                source_squared += source_delta * source_delta
    limit = 1 if case['lossy'] else 0
    return {'passed': reference_peak <= limit, 'reference_peak_limit': limit,
            'reference_kind': 'independent OpenJPEG PGM' if case['lossy'] else 'owner scalar oracle',
            'reference_peak_error': reference_peak, 'reference_mismatches': reference_mismatches,
            'reference_squared_error': reference_squared, 'source_peak_error': source_peak,
            'source_mismatches': source_mismatches, 'source_squared_error': source_squared,
            'decoded_pixels_sha256': pixel_digest(
                case, lambda x, y, band: planes[band][y * case['width'] + x])}


def run(args, report):
    owner, provenance = load_owner(args.testdata_source, args.input_pack)
    report['provenance'] = identity(args.input_pack / 'PROVENANCE.json')
    report['owner_contracts_sha256'] = OWNER_FILES
    lossy_case = next(case for case in provenance['cases'] if case['lossy'])
    # Bound before parsing: this comparator deliberately accepts only the locked
    # 68,705-sample PGM, never a generated large scene or arbitrary image.
    require(args.lossy_reference.stat().st_size <= lossy_case['samples'] * 2 + 4096,
            'Lossy reference exceeds the small locked-case byte limit')
    width, height, bands, values = owner.read_pnm(args.lossy_reference)
    require((width, height, bands) ==
            (lossy_case['width'], lossy_case['height'], lossy_case['bands']),
            'Lossy reference dimensions differ from the locked case')
    reference = array('H', values)
    del values
    reference_digest = pixel_digest(lossy_case, lambda x, y, band: reference[y * width + x])
    require(reference_digest == lossy_case['openjpeg_decoded_pixels_sha256'],
            'Normalised lossy reference pixels differ from the independent owner digest')
    report['lossy_reference'] = identity(args.lossy_reference)
    report['lossy_reference']['normalised_pixels_sha256'] = reference_digest
    report['binaries'] = {name: identity(getattr(args, name + '_library'))
                          for name in ('gdal', 'codec', 'plugin')}
    gdal = Gdal(args.gdal_library, args.codec_library, args.plugin_library)
    report['gdal_version'] = gdal.lib.GDALVersionInfo(b'RELEASE_NAME').decode()
    report['registration'] = 'explicit public NITF and JP2Emuella registration; no autoload'
    report['drivers'] = ['NITF', 'JP2Emuella']
    report['source_index_capability'] = 'REQUIRED_SUPPORTED'
    report['cases'] = []
    for case in provenance['cases']:
        planes, route = gdal.read_case(args.input_pack / (case['id'] + '.ntf'), case)
        result = {'id': case['id'], 'width': case['width'], 'height': case['height'],
                  'bits': case['bits'], 'bands': case['bands'], 'samples': case['samples'],
                  'coding': case['coding'], 'files': case['files'],
                  'source_pixels_sha256': case['source_pixels_sha256'],
                  'independent_pixels_sha256': case['openjpeg_decoded_pixels_sha256'],
                  **route, **compare(case, planes, owner.oracle, reference)}
        report['cases'].append(result)
        print(f"{case['id']}: reference peak={result['reference_peak_error']}, "
              f"mismatches={result['reference_mismatches']}, passed={result['passed']}")
    require(all(case['passed'] for case in report['cases']), 'Independent pixel comparison failed')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('gdal-library', 'codec-library', 'plugin-library', 'testdata-source',
                 'input-pack', 'lossy-reference', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    for name, value in vars(args).items():
        require(value.is_absolute(), f'--{name.replace("_", "-")} must be absolute')
    report = {'schema_version': 1, 'passed': False, 'testdata_revision': TESTDATA_REVISION,
              'pack': 'jpeg-2000/independent-nitf', 'pack_version': '1',
              'checker': identity(Path(__file__))}
    # Exclusive creation prevents a retry from replacing previous evidence.
    with args.output.open('x') as output:
        try:
            run(args, report)
            report['passed'] = True
        except Exception as error:
            report['error'] = str(error)
            print(f'Independent NITF check failed: {error}', file=sys.stderr)
        json.dump(report, output, indent=2, sort_keys=True)
        output.write('\n')
    print(f'Aggregate evidence: {args.output}')
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
