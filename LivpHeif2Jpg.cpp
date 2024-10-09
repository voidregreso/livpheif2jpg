#include <iostream>
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <thread>
#include <libheif/heif.h>
#include <zip.h>
#include <opencv2/opencv.hpp>
#include <exiv2/exiv2.hpp>

namespace fs = std::filesystem;

void ensure_dir_exists(const fs::path& directory) {
    fs::create_directories(directory);
}

bool is_file_type(const std::string& file_name, const std::vector<std::string>& extensions) {
    const auto func = [](std::string& str) {
        std::transform(str.begin(), str.end(), str.begin(), [](unsigned char c) { return std::tolower(c); });
    };
    for (const auto& ext : extensions) {
        std::string curExt = file_name.substr(file_name.size() - ext.size());
        func(curExt);
        if (file_name.size() >= ext.size() && curExt == ext) {
            return true;
        }
    }
    return false;
}

void convert_to_jpg(const std::string& img_id, const cv::Mat& image, const fs::path& output_dir, int quality = 90) {
    cv::imwrite((output_dir / (img_id + ".jpg")).string(), image, { cv::IMWRITE_JPEG_QUALITY, quality });
}

void extract_jpeg(const std::string& img_id, const std::vector<uint8_t>& jpeg_data, const fs::path& output_dir) {
    std::ofstream outfile((output_dir / (img_id + ".jpg")).string(), std::ios::binary);
    outfile.write(reinterpret_cast<const char*>(jpeg_data.data()), jpeg_data.size());
}

std::vector<uint8_t> extract_exif_from_heif(heif_image_handle* handle) {
    std::vector<uint8_t> exif_data;
    heif_item_id exif_id;
    int n = heif_image_handle_get_list_of_metadata_block_IDs(handle, "Exif", &exif_id, 1);
    if (n == 1) {
        size_t exif_size = heif_image_handle_get_metadata_size(handle, exif_id);
        exif_data.resize(exif_size);
        struct heif_error error = heif_image_handle_get_metadata(handle, exif_id, exif_data.data());
        if (error.code != heif_error_Ok) {
            std::cerr << "Failed to extract EXIF data: " << error.message << std::endl;
            exif_data.clear();
        }
    }
    return exif_data;
}

void add_exif_to_jpg(const std::string& jpg_path, const std::vector<uint8_t>& exif_data) {
    try {
        auto image = Exiv2::ImageFactory::open(jpg_path);
        if (image.get() != 0 && !exif_data.empty()) {
            Exiv2::ExifData exif;

            // Check byte order
            // bool is_little_endian = (exif_data[10] == 0x49 && exif_data[11] == 0x49);

            // Create a new buffer starting from the TIFF header
            // skip first 4 + 6 bytes (HEIF 0x00,00,00,06; EXIF 0x45,78,69,66,00,00)
            // in order to make TIFF header (0x4d,4d,00,2a) appear at the very beginning of the binary data
            std::vector<uint8_t> tiff_data(exif_data.begin() + 10, exif_data.end());

            /*// Ensure TIFF header is correct
            tiff_data[0] = tiff_data[1] = (is_little_endian ? 0x49 : 0x4D);
            tiff_data[2] = (is_little_endian ? 0x2A : 0x00);
            tiff_data[3] = (is_little_endian ? 0x00 : 0x2A);

            if (is_little_endian) {
                // Convert data after TIFF header from little-endian to big-endian
                for (size_t i = 4; i < tiff_data.size() - 1; i += 2) {
                    std::swap(tiff_data[i], tiff_data[i + 1]);
                }
            }*/

            Exiv2::ExifParser::decode(exif, tiff_data.data(), tiff_data.size());

            // Set the orientation to normal (1), i.e., DO NOT rotate
            exif["Exif.Image.Orientation"] = 1;

            image->setExifData(exif);
            image->writeMetadata();
        }
    }
    catch (Exiv2::Error& e) {
        std::cerr << "Exiv2 exception in add_exif_to_jpg: " << e.what() << std::endl;
        std::ofstream outFile(jpg_path + "_origexif.bin", std::ios::binary);
        outFile.write(reinterpret_cast<const char*>(exif_data.data()), exif_data.size());
        outFile.close();
        std::cerr << "It's original EXIF data has been written to another file." << std::endl;
    }
}

void process_heif_image(const std::string& img_id, const std::vector<uint8_t>& buffer, const fs::path& output_dir, int quality) {
    heif_context* ctx = heif_context_alloc();
    heif_context_read_from_memory(ctx, buffer.data(), buffer.size(), nullptr);

    heif_image_handle* handle;
    heif_context_get_primary_image_handle(ctx, &handle);

    // Extract EXIF data
    std::vector<uint8_t> exif_data = extract_exif_from_heif(handle);

    heif_image* img;
    heif_decode_image(handle, &img, heif_colorspace_RGB, heif_chroma_interleaved_RGB, nullptr);

    int width = heif_image_get_width(img, heif_channel_interleaved);
    int height = heif_image_get_height(img, heif_channel_interleaved);
    int stride;
    const uint8_t* data = heif_image_get_plane_readonly(img, heif_channel_interleaved, &stride);

    cv::Mat image(height, width, CV_8UC3, (void*)data, stride);
    cv::cvtColor(image, image, cv::COLOR_RGB2BGR);

    fs::path output_path = output_dir / (img_id + ".jpg");
    convert_to_jpg(img_id, image, output_dir, quality);

    // Add EXIF data to the converted JPG
    add_exif_to_jpg(output_path.string(), exif_data);

    heif_image_release(img);
    heif_image_handle_release(handle);
    heif_context_free(ctx);
}

void livp_to_jpg(const std::string& img_item, const fs::path& img_source, const fs::path& output_dir, int quality = 90) {
    std::string img_id = img_item.substr(0, img_item.find_last_of('.'));

    int err = 0;
    zip* z = zip_open(img_source.generic_string().c_str(), 0, &err);
    if (z == nullptr) {
        std::cerr << "Failed to open LIVP file: " << img_source << std::endl;
        return;
    }

    std::vector<std::string> heif_extensions = { ".heif", ".heic" };
    std::vector<std::string> jpeg_extensions = { ".jpg", ".jpeg" };

    for (int i = 0; i < zip_get_num_entries(z, 0); ++i) {
        struct zip_stat st;
        zip_stat_index(z, i, 0, &st);

        if (is_file_type(st.name, heif_extensions)) {
            struct zip_file* zf = zip_fopen_index(z, i, 0);
            if (zf) {
                std::vector<uint8_t> buffer(st.size);
                zip_fread(zf, buffer.data(), st.size);
                zip_fclose(zf);

                process_heif_image(img_id, buffer, output_dir, quality);
                break;
            }
        }
        else if (is_file_type(st.name, jpeg_extensions)) {
            struct zip_file* zf = zip_fopen_index(z, i, 0);
            if (zf) {
                std::vector<uint8_t> buffer(st.size);
                zip_fread(zf, buffer.data(), st.size);
                zip_fclose(zf);

                extract_jpeg(img_id, buffer, output_dir);
                break;
            }
        }
    }

    zip_close(z);
}

void heif_to_jpg(const std::string& img_item, const fs::path& img_source, const fs::path& output_dir, int quality = 90) {
    std::string img_id = img_item.substr(0, img_item.find_last_of('.'));

    std::ifstream file(img_source, std::ios::binary | std::ios::ate);
    std::vector<uint8_t> buffer(file.tellg());
    file.seekg(0, std::ios::beg);
    file.read(reinterpret_cast<char*>(buffer.data()), buffer.size());

    process_heif_image(img_id, buffer, output_dir, quality);
}

void process_image(const std::string& img_item, const fs::path& input_dir, const fs::path& output_dir, int quality) {
    fs::path img_source = input_dir / img_item;
    if (is_file_type(img_item, { ".livp" })) {
        livp_to_jpg(img_item, img_source, output_dir, quality);
    }
    else if (is_file_type(img_item, { ".heif", ".heic" })) {
        heif_to_jpg(img_item, img_source, output_dir, quality);
    }
}

int main(int argc, char* argv[]) {
    int num_cores = std::thread::hardware_concurrency();
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <input_dir> <output_dir> [quality=93] [threads=" << num_cores << "]" << std::endl;
        return 1;
    }

    fs::path input_dir = argv[1];
    fs::path output_dir = argv[2];
    int quality = (argc > 3) ? std::stoi(argv[3]) : 90;
    int num_threads = (argc > 4) ? std::stoi(argv[4]) : num_cores;

    ensure_dir_exists(output_dir);

    std::vector<std::string> img_list;
    for (const auto& entry : fs::directory_iterator(input_dir)) {
        if (is_file_type(entry.path().filename().string(), { ".livp", ".heif", ".heic" })) {
            img_list.push_back(entry.path().filename().string());
        }
    }

    std::vector<std::thread> threads;
    for (size_t i = 0; i < img_list.size(); i += num_threads) {
        for (int j = 0; j < num_threads && i + j < img_list.size(); ++j) {
            threads.emplace_back(process_image, img_list[i + j], input_dir, output_dir, quality);
        }
        for (auto& t : threads) {
            t.join();
        }
        threads.clear();

        std::cout << "\rProgress: " << std::min(i + num_threads, img_list.size()) << "/" << img_list.size()
            << " (" << (std::min(i + num_threads, img_list.size()) * 100 / img_list.size()) << "%)" << std::flush;
    }
    std::cout << std::endl;

    return 0;
}
