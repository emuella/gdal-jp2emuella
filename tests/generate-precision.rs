// Project-authored arithmetic images; regenerate with the exact codec revision
// recorded in precision-PROVENANCE.toml. No external image inputs are used.
use emuella_j2k::{
    ColorModel, ComponentLayout, EncodeOptions, ImageInfo, ImageView, OutputFormat, SampleEndian,
    SampleFormat, TileSize, encode,
};
fn main() {
    let output = std::path::PathBuf::from(std::env::args_os().nth(1).expect("output directory"));
    std::fs::create_dir_all(&output).unwrap();
    for bits in [16] {
        for bands in [1_u16, 3] {
            let format =
                SampleFormat::with_byte_order(bits, false, Some(SampleEndian::Little)).unwrap();
            let info = ImageInfo::new(
                67,
                53,
                bands,
                format,
                if bands == 1 {
                    ColorModel::Grayscale
                } else {
                    ColorModel::Rgb
                },
                ComponentLayout::Interleaved,
            )
            .unwrap();
            let mask = (1_u32 << bits) - 1;
            let mut samples = Vec::new();
            for y in 0..53_u32 {
                for x in 0..67_u32 {
                    for band in 0..u32::from(bands) {
                        let value = if x == 0 && y == 0 {
                            0
                        } else if x == 66 && y == 52 {
                            mask
                        } else {
                            (x * 997 + y * 617 + x * y * 13 + band * 1237) & mask
                        };
                        samples.extend_from_slice(&(value as u16).to_le_bytes());
                    }
                }
            }
            let options = EncodeOptions {
                format: OutputFormat::J2kCodestream,
                decomposition_levels: 2,
                tile_size: Some(TileSize {
                    width: 32,
                    height: 32,
                }),
                ..EncodeOptions::default()
            };
            let bytes = encode(
                ImageView::Interleaved {
                    info: &info,
                    samples: &samples,
                    stride_bytes: 67 * usize::from(bands) * 2,
                },
                &options,
            )
            .unwrap();
            std::fs::write(output.join(format!("precision-{bits}-{bands}.j2k")), bytes).unwrap();
        }
    }
}
