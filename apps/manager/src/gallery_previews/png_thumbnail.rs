use super::{MAX_DECODE_BYTES, MAX_IMAGE_SIDE, PREVIEW_HEIGHT, PREVIEW_WIDTH};
use image::{Rgba, RgbaImage};
use png::{BitDepth, ColorType, Decoder, DecodingError, Reader, Transformations};
use std::io::{self, BufRead, Seek};

pub(super) fn load<R: BufRead + Seek>(input: R) -> Result<Option<RgbaImage>, DecodingError> {
    let mut decoder = Decoder::new_with_limits(
        input,
        png::Limits {
            bytes: MAX_DECODE_BYTES,
        },
    );
    decoder.set_transformations(Transformations::EXPAND);
    let info = decoder.read_header_info()?;
    let (width, height) = (info.width, info.height);
    let scale = width / PREVIEW_WIDTH;
    if scale < 2
        || width != PREVIEW_WIDTH * scale
        || height != PREVIEW_HEIGHT * scale
        || width > MAX_IMAGE_SIDE
        || height > MAX_IMAGE_SIDE
        || info.interlaced
        || info.bit_depth != BitDepth::Eight
        || !matches!(info.color_type, ColorType::Rgb | ColorType::Rgba)
    {
        return Ok(None);
    }
    let mut reader = decoder.read_info()?;
    if reader.info().animation_control.is_some() {
        return Ok(None);
    }
    let bytes = u64::from(width) * u64::from(height);
    let size = scale as usize;
    match reader.output_color_type() {
        (ColorType::Rgb, BitDepth::Eight) if bytes * 3 <= MAX_DECODE_BYTES as u64 => {
            pixels::<3, _>(&mut reader, size).map(Some)
        }
        (ColorType::Rgba, BitDepth::Eight) if bytes * 4 <= MAX_DECODE_BYTES as u64 => {
            pixels::<4, _>(&mut reader, size).map(Some)
        }
        _ => Ok(None),
    }
}

fn pixels<const CHANNELS: usize, R: BufRead + Seek>(
    reader: &mut Reader<R>,
    scale: usize,
) -> Result<RgbaImage, DecodingError> {
    let mut cover = RgbaImage::new(PREVIEW_WIDTH, PREVIEW_HEIGHT);
    let samples = (scale * scale) as u32;
    let block = scale * CHANNELS;
    let row_bytes = PREVIEW_WIDTH as usize * block;
    // Integer downsampling uses the same channel sums and rounding as image::thumbnail.
    for output in cover.rows_mut() {
        let mut sums = [[0u32; CHANNELS]; PREVIEW_WIDTH as usize];
        for _ in 0..scale {
            let row = reader.next_row()?.ok_or_else(|| {
                io::Error::new(io::ErrorKind::UnexpectedEof, "Incomplete PNG row")
            })?;
            if row.data().len() != row_bytes {
                return Err(
                    io::Error::new(io::ErrorKind::InvalidData, "Incorrect PNG row width").into(),
                );
            }
            for (sum, input) in sums.iter_mut().zip(row.data().chunks_exact(block)) {
                for pixel in input.chunks_exact(CHANNELS) {
                    for (channel, value) in sum.iter_mut().zip(pixel) {
                        *channel += u32::from(*value);
                    }
                }
            }
        }
        for (dest, sum) in output.zip(sums) {
            let mut rgba = [255; 4];
            for (channel, total) in rgba.iter_mut().zip(sum) {
                *channel = ((total + samples / 2) / samples) as u8;
            }
            *dest = Rgba(rgba);
        }
    }
    // Flush the IDAT stream and read its CRC even though all output rows are available.
    if reader.next_row()?.is_some() {
        return Err(io::Error::new(io::ErrorKind::InvalidData, "Unexpected PNG row").into());
    }
    Ok(cover)
}

#[cfg(test)]
mod tests {
    use super::*;
    use image::{DynamicImage, ImageFormat, Rgb, RgbImage};
    use std::io::Cursor;

    fn png(image: &DynamicImage) -> Vec<u8> {
        let mut output = Cursor::new(Vec::new());
        image.write_to(&mut output, ImageFormat::Png).unwrap();
        output.into_inner()
    }

    #[test]
    fn streamed_rgb_and_transparent_rgba_match_the_existing_box_filter() {
        let rgb = RgbImage::from_fn(960, 360, |x, y| {
            Rgb([(x + y) as u8, (x * 3 + y * 5) as u8, (y * 9 + x) as u8])
        });
        let rgba = RgbaImage::from_fn(1440, 540, |x, y| {
            Rgba([
                (x + y) as u8,
                (x * 3 + y * 5) as u8,
                (y * 9 + x) as u8,
                (x * y) as u8,
            ])
        });
        for source in [DynamicImage::ImageRgb8(rgb), DynamicImage::ImageRgba8(rgba)] {
            let bytes = png(&source);
            let expected = source.thumbnail(PREVIEW_WIDTH, PREVIEW_HEIGHT).into_rgba8();
            let actual = load(Cursor::new(&bytes)).unwrap().unwrap();
            assert_eq!(actual, expected);
        }
    }

    #[test]
    fn native_size_and_non_integer_sizes_use_the_normal_decoder() {
        for (width, height) in [(480, 180), (1024, 768), (961, 360)] {
            let bytes = png(&DynamicImage::ImageRgba8(RgbaImage::new(width, height)));
            assert!(load(Cursor::new(&bytes)).unwrap().is_none());
        }
    }
}
