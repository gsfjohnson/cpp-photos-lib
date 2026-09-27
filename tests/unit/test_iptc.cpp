#include "testing.hpp"

using namespace lumenlib;

TEST(iptc_tags_by_iim_name) {
  const IptcTag t("Keywords");
  CHECK_EQ(t.record(), 2);
  CHECK_EQ(t.dataset(), 25);
  CHECK_EQ(t.str(), "Keywords");
  CHECK_EQ(IptcTag(1, 90).str(), "CodedCharacterSet");
  CHECK_EQ(IptcTag(2, 120).str(), "CaptionAbstract");
  CHECK_EQ(IptcTag(2, 80).str(), "ByLine");
  CHECK_EQ(IptcTag(2, 250).str(), "2:250");
  CHECK(IptcTag("2:250") == IptcTag(2, 250));
  CHECK(IptcTag("2:25") == IptcTag("Keywords"));
  CHECK_THROWS(IptcTag("Nope"), ErrorCode::invalidArgument);
  CHECK_THROWS(IptcTag("2:256"), ErrorCode::invalidArgument);
  // Every name is unique across the records.
  for (const auto& a : iptcDatasetTable()) {
    CHECK(lookupIptcDataset(a.name) == &a);
  }
}

TEST(iptc_round_trip) {
  IptcMetadata iptc;
  iptc.append("Keywords", "harbour");
  iptc.append("Keywords", testing::kKobenhavn);
  iptc.set("CaptionAbstract", "A caption");
  const Bytes encoded = iptc.encode();
  const IptcMetadata back = IptcMetadata::decode(encoded.data(), encoded.size());
  CHECK((back.values("Keywords") == std::vector<std::string>{"harbour", testing::kKobenhavn}));
  CHECK_EQ(*back.value("CaptionAbstract"), "A caption");
  // Non-ASCII text declares UTF-8, and a record version is added.
  CHECK_EQ(*back.value("CodedCharacterSet"), "\x1b%G");
  CHECK_EQ(*back.value("RecordVersion"), "4");
  CHECK(!back.value("City"));
}

TEST(iptc_latin1_is_converted) {
  // 2:25 "K\xf8benhavn" in ISO 8859-1, no character set declared.
  const Bytes raw = {0x1c, 2, 25, 0, 9, 'K', 0xf8, 'b', 'e', 'n', 'h', 'a', 'v', 'n'};
  const IptcMetadata iptc = IptcMetadata::decode(raw.data(), raw.size());
  CHECK_EQ(*iptc.value("Keywords"), testing::kKobenhavn);
}

TEST(iptc_extended_length) {
  IptcMetadata iptc;
  iptc.set("CaptionAbstract", std::string(40000, 'x'));
  const Bytes encoded = iptc.encode();
  const IptcMetadata back = IptcMetadata::decode(encoded.data(), encoded.size());
  CHECK_EQ(back.value("CaptionAbstract")->size(), 40000u);
}

TEST(iptc_set_values_keeps_position) {
  IptcMetadata iptc;
  iptc.append("ObjectName", "t");
  iptc.append("Keywords", "a");
  iptc.append("City", "c");
  iptc.setValues("Keywords", {"x", "y"});
  iptc.append("Keywords", "z");  // after the other keywords
  std::vector<std::string> entries;
  for (const auto& e : iptc) entries.push_back(e.name() + "=" + e.value());
  CHECK((entries == std::vector<std::string>{"ObjectName=t", "Keywords=x", "Keywords=y", "Keywords=z", "City=c"}));
  iptc.setValues("Keywords", {});
  CHECK(iptc.values("Keywords").empty());
  CHECK_EQ(iptc.remove("City"), 1u);
  CHECK_EQ(iptc.size(), 1u);
}

TEST(iptc_truncated_data_is_an_error) {
  const Bytes raw = {0x1c, 2, 25, 0, 20, 'a', 'b'};
  CHECK_THROWS(IptcMetadata::decode(raw.data(), raw.size()), ErrorCode::corruptData);
  const Bytes badExtended = {0x1c, 2, 25, 0x80, 0x09, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  CHECK_THROWS(IptcMetadata::decode(badExtended.data(), badExtended.size()), ErrorCode::corruptData);
}

TEST(iptc_binary_short_datasets) {
  IptcMetadata iptc;
  iptc.set("RecordVersion", "70000");
  CHECK_THROWS(iptc.encode(), ErrorCode::invalidArgument);
  iptc.set("RecordVersion", "2");
  const Bytes encoded = iptc.encode();
  CHECK_EQ(encoded.size(), 7u);
  CHECK_EQ(encoded[5], 0);
  CHECK_EQ(encoded[6], 2);
}
