import os
from PIL import Image
import zipfile
import argparse
from tqdm import tqdm
import pillow_heif
from concurrent.futures import ThreadPoolExecutor

def ensure_dir_exists(directory):
    os.makedirs(directory, exist_ok=True)

def is_file_type(file_name, extensions):
    return file_name.lower().endswith(extensions)

def convert_to_jpg(img_id, heif_file, output_dir, quality=90):
    image = Image.frombytes(heif_file.mode, heif_file.size, heif_file.data.tobytes(), "raw")
    image.save(os.path.join(output_dir, img_id + '.jpg'), format="jpeg", quality=quality)

def extract_jpeg(img_id, jpeg_data, output_dir):
    with open(os.path.join(output_dir, img_id + '.jpg'), 'wb') as f:
        f.write(jpeg_data)

def livp_to_jpg(img_item, img_source, output_dir, quality=90):
    img_id = os.path.splitext(img_item)[0]
    with zipfile.ZipFile(img_source) as zf:
        for zip_file_item in zf.namelist():
            if is_file_type(zip_file_item, (".heif", ".heic")):
                convert_to_jpg(img_id, pillow_heif.read_heif(zf.read(zip_file_item)), output_dir, quality)
                break
            elif is_file_type(zip_file_item, (".jpg", ".jpeg")):
                extract_jpeg(img_id, zf.read(zip_file_item), output_dir)
                break

def heif_to_jpg(img_item, img_source, output_dir, quality=90):
    img_id = os.path.splitext(img_item)[0]
    convert_to_jpg(img_id, pillow_heif.read_heif(img_source), output_dir, quality)

def process_image(img_item, input_dir, output_dir, quality):
    img_source = os.path.join(input_dir, img_item)
    if is_file_type(img_item, ".livp"):
        livp_to_jpg(img_item, img_source, output_dir, quality)
    elif is_file_type(img_item, (".heif", ".heic")):
        heif_to_jpg(img_item, img_source, output_dir, quality)

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Convert LIVP and HEIF images to JPG.')
    parser.add_argument('--input_dir', type=str, required=True, help='Path to the input directory containing LIVP or HEIF files.')
    parser.add_argument('--output_dir', type=str, required=True, help='Path to the output directory for converted JPG files.')
    parser.add_argument('--quality', type=int, default=95, help='Quality of the output JPG images (1-100).')
    parser.add_argument('--threads', type=int, default=4, help='Number of threads to use for concurrent processing.')
    args = parser.parse_args()

    ensure_dir_exists(args.output_dir)

    img_list = [f for f in os.listdir(args.input_dir) if is_file_type(f, (".livp", ".heif", ".heic"))]

    with ThreadPoolExecutor(max_workers=args.threads) as executor:
        list(tqdm(executor.map(lambda img_item: process_image(img_item, args.input_dir, args.output_dir, args.quality), img_list), total=len(img_list)))
