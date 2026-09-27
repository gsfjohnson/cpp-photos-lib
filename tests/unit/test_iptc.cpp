#include "testing.hpp"

using namespace photos;

TEST(iptc_keys) {
  const IptcKey k("Iptc.Application2.Keywords");
  CHECK_EQ(k.record(), 2);
  CHECK_EQ(k.dataset(), 25);
  CHECK_EQ(k.str(), "Iptc.Application2.Keywords");
  CHECK_EQ(IptcKey(1, 90).str(), "Iptc.Envelope.CharacterSet");
  CHECK_EQ(IptcKey(2, 250).str(), "Iptc.Application2.0x00fa");
  CHECK_EQ(IptcKey("Iptc.Application2.0x00fa").dataset(), 250);
  CHECK_THROWS(IptcKey("Iptc.Application2.Nope"), ErrorCode::invalidArgument);
}

TEST(iptc_round_trip) {
  IptcData iptc;
  iptc.add("Iptc.Application2.Keywords", "harbour");
  iptc.add("Iptc.Application2.Keywords", testing::kKobenhavn);
  iptc["Iptc.Application2.Caption"] = "A caption";
  const Bytes encoded = iptc.encode();
  const IptcData back = IptcData::decode(encoded.data(), encoded.size());
  CHECK((back.values("Iptc.Application2.Keywords") == std::vector<std::string>{"harbour", testing::kKobenhavn}));
  CHECK_EQ(back.find("Iptc.Application2.Caption")->toString(), "A caption");
  // Non-ASCII text declares UTF-8, and a record version is added.
  CHECK_EQ(back.find("Iptc.Envelope.CharacterSet")->toString(), "\x1b%G");
  CHECK_EQ(back.find("Iptc.Application2.RecordVersion")->toString(), "4");
}

TEST(iptc_latin1_is_converted) {
  // 2:25 "K\xf8benhavn" in ISO 8859-1, no character set declared.
  const Bytes raw = {0x1c, 2, 25, 0, 9, 'K', 0xf8, 'b', 'e', 'n', 'h', 'a', 'v', 'n'};
  const IptcData iptc = IptcData::decode(raw.data(), raw.size());
  CHECK_EQ(iptc.find("Iptc.Application2.Keywords")->toString(), testing::kKobenhavn);
}

TEST(iptc_extended_length) {
  IptcData iptc;
  iptc["Iptc.Application2.Caption"] = std::string(40000, 'x');
  const Bytes encoded = iptc.encode();
  const IptcData back = IptcData::decode(encoded.data(), encoded.size());
  CHECK_EQ(back.find("Iptc.Application2.Caption")->toString().size(), 40000u);
}

TEST(iptc_set_values_keeps_position) {
  IptcData iptc;
  iptc.add("Iptc.Application2.ObjectName", "t");
  iptc.add("Iptc.Application2.Keywords", "a");
  iptc.add("Iptc.Application2.City", "c");
  iptc.setValues("Iptc.Application2.Keywords", {"x", "y"});
  std::vector<std::string> keys;
  for (const auto& d : iptc) keys.push_back(d.tagName() + "=" + d.toString());
  CHECK((keys == std::vector<std::string>{"ObjectName=t", "Keywords=x", "Keywords=y", "City=c"}));
  iptc.setValues("Iptc.Application2.Keywords", {});
  CHECK(iptc.values("Iptc.Application2.Keywords").empty());
}

TEST(iptc_truncated_data_is_an_error) {
  const Bytes raw = {0x1c, 2, 25, 0, 20, 'a', 'b'};
  CHECK_THROWS(IptcData::decode(raw.data(), raw.size()), ErrorCode::corruptData);
  const Bytes badExtended = {0x1c, 2, 25, 0x80, 0x09, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  CHECK_THROWS(IptcData::decode(badExtended.data(), badExtended.size()), ErrorCode::corruptData);
}

TEST(iptc_binary_short_datasets) {
  IptcData iptc;
  iptc["Iptc.Application2.RecordVersion"] = "70000";
  CHECK_THROWS(iptc.encode(), ErrorCode::invalidArgument);
  iptc["Iptc.Application2.RecordVersion"] = "2";
  const Bytes encoded = iptc.encode();
  CHECK_EQ(encoded.size(), 7u);
  CHECK_EQ(encoded[5], 0);
  CHECK_EQ(encoded[6], 2);
}
