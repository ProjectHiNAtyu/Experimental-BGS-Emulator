#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace bgsemu::local_resources
{
    inline constexpr uint32_t program = 0x0000424E;
    inline constexpr uint32_t stream = 0x61706674;
    inline constexpr char path[] = "/resources/628e6c5c3b951f71a9e3a2c831aeb0c809b5d3679fcef14dfe3ba17db6c8045c";
    inline constexpr char url[] = "http://nydus.battle.net:9090/resources/628e6c5c3b951f71a9e3a2c831aeb0c809b5d3679fcef14dfe3ba17db6c8045c";
    inline const std::string body = std::string(
        "\x5a\x70\x6d\x43\x00\x00\x00\x00\x0d\x00\x00\x00\x00\x00\x00\x00\x78\x01\x01\x0d\x00\xf2\xff\x0a\x0b\x0a\x03\x62\x61\x64\x12\x04\x28\x3f\x21\x29\x0d\x1d\x02\x11", 40);
    inline constexpr std::array<unsigned char, 32> hash = {
        0x62,0x8e,0x6c,0x5c,0x3b,0x95,0x1f,0x71,0xa9,0xe3,0xa2,0xc8,0x31,0xae,0xb0,0xc8,
        0x09,0xb5,0xd3,0x67,0x9f,0xce,0xf1,0x4d,0xfe,0x3b,0xa1,0x7d,0xb6,0xc8,0x04,0x5c
    };
}
