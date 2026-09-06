#pragma once

#include <optional>
#include <span>
#include <string_view>

#include "liquid/format.hpp"

namespace liquid::formats::detail {

class codec_backend {
public:
    virtual ~codec_backend() = default;

    virtual auto recognise(std::span<const std::byte> blob, std::string_view path)
        -> result<std::optional<format_recognition>> = 0;
    virtual auto unpack(std::span<const std::byte> blob, format_limits limits)
        -> result<format_tree> = 0;
    virtual auto canonicalise(const format_tree& tree) -> result<format_tree>;
    virtual auto diff(const format_tree& old_tree,
                      const format_tree& new_tree,
                      format_diff_options options) -> result<format_diff>;
    virtual auto merge(const format_tree& base, const format_tree& left, const format_tree& right)
        -> result<format_tree>;
    virtual auto repack(const format_tree& tree) -> result<std::vector<std::byte>>;
    virtual auto volatile_fields() const -> std::span<const std::string_view> = 0;
};

[[nodiscard]] auto make_entry(std::string path,
                              format_entry_kind kind,
                              std::vector<std::byte> bytes,
                              std::vector<std::byte> attributes = {},
                              std::optional<format_byte_range> range = {}) -> result<format_entry>;
[[nodiscard]] auto empty_attributes() -> result<std::vector<std::byte>>;
[[nodiscard]] auto attributes(std::initializer_list<std::pair<std::string, std::string>> fields)
    -> result<std::vector<std::byte>>;

int recognise_operation(void* module_context,
                        liquid_format_bytes_v1 input_cbor,
                        const liquid_format_callbacks_v1* callbacks,
                        liquid_format_owned_bytes_v1* output_cbor,
                        liquid_format_error_v1* error);
int unpack_operation(void* module_context,
                     liquid_format_bytes_v1 input_cbor,
                     const liquid_format_callbacks_v1* callbacks,
                     liquid_format_owned_bytes_v1* output_cbor,
                     liquid_format_error_v1* error);
int canonicalise_operation(void* module_context,
                           liquid_format_bytes_v1 input_cbor,
                           const liquid_format_callbacks_v1* callbacks,
                           liquid_format_owned_bytes_v1* output_cbor,
                           liquid_format_error_v1* error);
int diff_operation(void* module_context,
                   liquid_format_bytes_v1 input_cbor,
                   const liquid_format_callbacks_v1* callbacks,
                   liquid_format_owned_bytes_v1* output_cbor,
                   liquid_format_error_v1* error);
int merge_operation(void* module_context,
                    liquid_format_bytes_v1 input_cbor,
                    const liquid_format_callbacks_v1* callbacks,
                    liquid_format_owned_bytes_v1* output_cbor,
                    liquid_format_error_v1* error);
int repack_operation(void* module_context,
                     liquid_format_bytes_v1 input_cbor,
                     const liquid_format_callbacks_v1* callbacks,
                     liquid_format_owned_bytes_v1* output_cbor,
                     liquid_format_error_v1* error);

[[nodiscard]] auto zip_module() -> const liquid_format_module_v1*;
[[nodiscard]] auto tar_module() -> const liquid_format_module_v1*;
[[nodiscard]] auto hdf5_module() -> const liquid_format_module_v1*;
[[nodiscard]] auto parquet_module() -> const liquid_format_module_v1*;
[[nodiscard]] auto sqlite_module() -> const liquid_format_module_v1*;
[[nodiscard]] auto deb_module() -> const liquid_format_module_v1*;
[[nodiscard]] auto rpm_module() -> const liquid_format_module_v1*;
[[nodiscard]] auto apk_module() -> const liquid_format_module_v1*;
[[nodiscard]] auto elf_module() -> const liquid_format_module_v1*;
[[nodiscard]] auto macho_module() -> const liquid_format_module_v1*;
[[nodiscard]] auto pe_module() -> const liquid_format_module_v1*;
[[nodiscard]] auto jupyter_module() -> const liquid_format_module_v1*;
[[nodiscard]] auto safetensors_module() -> const liquid_format_module_v1*;
[[nodiscard]] auto pickle_module() -> const liquid_format_module_v1*;
[[nodiscard]] auto pdf_module() -> const liquid_format_module_v1*;
[[nodiscard]] auto ooxml_module() -> const liquid_format_module_v1*;
[[nodiscard]] auto odf_module() -> const liquid_format_module_v1*;
[[nodiscard]] auto wheel_module() -> const liquid_format_module_v1*;
[[nodiscard]] auto jar_module() -> const liquid_format_module_v1*;

} // namespace liquid::formats::detail
