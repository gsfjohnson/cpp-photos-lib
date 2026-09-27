#include "photoshop.hpp"

#include "bytes.hpp"

#include <algorithm>

namespace lumenlib::detail {
namespace {

bool isResourceSignature(std::uint32_t sig) {
  // 8BIM, and the variants other applications use.
  return sig == 0x3842494d || sig == 0x50485554 /* PHUT */ || sig == 0x41674867 /* AgHg */ ||
         sig == 0x44435352 /* DCSR */;
}

}  // namespace

std::vector<ImageResource> parseImageResources(const std::uint8_t* data, std::size_t size) {
  std::vector<ImageResource> out;
  std::size_t pos = 0;
  while (inBounds(size, pos, 4 + 2 + 1)) {
    const std::uint32_t sig = getBe32(data + pos);
    if (!isResourceSignature(sig)) break;
    ImageResource r;
    r.signature = sig;
    r.id = getBe16(data + pos + 4);
    pos += 6;
    const std::size_t nameLength = data[pos];
    std::size_t nameField = 1 + nameLength;
    nameField += nameField & 1;
    if (!inBounds(size, pos, nameField + 4)) break;
    r.name.assign(data + pos, data + pos + 1 + nameLength);
    pos += nameField;
    const std::uint32_t length = getBe32(data + pos);
    pos += 4;
    if (!inBounds(size, pos, length)) break;
    r.data.assign(data + pos, data + pos + length);
    pos += length + (length & 1);
    out.push_back(std::move(r));
  }
  return out;
}

Bytes serializeImageResources(const std::vector<ImageResource>& resources) {
  Bytes out;
  for (const auto& r : resources) {
    appendBe32(out, r.signature);
    appendBe16(out, r.id);
    if (r.name.empty()) {
      out.push_back(0);
      out.push_back(0);
    } else {
      append(out, r.name);
      if (r.name.size() & 1) out.push_back(0);
    }
    appendBe32(out, static_cast<std::uint32_t>(r.data.size()));
    append(out, r.data);
    if (r.data.size() & 1) out.push_back(0);
  }
  return out;
}

std::optional<IptcMetadata> iptcFromImageResources(const std::vector<ImageResource>& resources) {
  for (const auto& r : resources) {
    if (r.id == kIptcResource && r.signature == 0x3842494d) return IptcMetadata::decode(r.data.data(), r.data.size());
  }
  return std::nullopt;
}

void setIptcImageResource(std::vector<ImageResource>& resources, const IptcMetadata& iptc) {
  const Bytes encoded = iptc.encode();
  resources.erase(
      std::remove_if(resources.begin(), resources.end(),
                     [](const ImageResource& r) { return r.id == kIptcResource || r.id == kIptcDigestResource; }),
      resources.end());
  if (encoded.empty()) return;
  resources.push_back({0x3842494d, kIptcResource, {}, encoded});
  const auto digest = md5(encoded.data(), encoded.size());
  resources.push_back({0x3842494d, kIptcDigestResource, {}, Bytes(digest.begin(), digest.end())});
}

}  // namespace lumenlib::detail
