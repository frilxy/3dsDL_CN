#include "timezone.hpp"

#include <cstdio>
#include <cstdlib>

namespace tz {

const std::vector<Zone> &zones()
{
    static const std::vector<Zone> all = {
        {-720, "贝克岛"},
        {-660, "美属萨摩亚、纽埃"},
        {-600, "夏威夷、大溪地"},
        {-570, "马克萨斯群岛"},
        {-540, "阿拉斯加"},
        {-480, "太平洋时间（美国、加拿大）"},
        {-420, "山地时间（美国、加拿大）"},
        {-360, "中部时间（美国、加拿大）、墨西哥城"},
        {-300, "东部时间（美国、加拿大）、哥伦比亚、秘鲁"},
        {-240, "大西洋时间、委内瑞拉、玻利维亚"},
        {-210, "纽芬兰"},
        {-180, "巴西、阿根廷、乌拉圭"},
        {-120, "南乔治亚岛"},
        {-60, "亚速尔群岛、佛得角"},
        {0, "英国、爱尔兰、葡萄牙、冰岛、加纳"},
        {60, "中欧、尼日利亚、阿尔及利亚"},
        {120, "东欧、埃及、南非"},
        {180, "莫斯科、土耳其、沙特阿拉伯、肯尼亚"},
        {210, "伊朗"},
        {240, "阿联酋、阿曼、阿塞拜疆"},
        {270, "阿富汗"},
        {300, "巴基斯坦、乌兹别克斯坦、马尔代夫"},
        {330, "印度、斯里兰卡"},
        {345, "尼泊尔"},
        {360, "孟加拉国、不丹"},
        {390, "缅甸"},
        {420, "泰国、越南、雅加达"},
        {480, "中国、新加坡、菲律宾、珀斯"},
        {525, "尤克拉"},
        {540, "日本、韩国"},
        {570, "阿德莱德、达尔文"},
        {600, "悉尼、布里斯班、关岛"},
        {630, "豪勋爵岛"},
        {660, "所罗门群岛、新喀里多尼亚"},
        {720, "新西兰、斐济"},
        {765, "查塔姆群岛"},
        {780, "汤加、萨摩亚"},
        {840, "莱恩群岛（基里巴斯）"},
    };
    return all;
}

std::string formatOffset(int offsetMinutes)
{
    const int magnitude = std::abs(offsetMinutes);
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "UTC%c%02d:%02d",
                  offsetMinutes < 0 ? '-' : '+', magnitude / 60,
                  magnitude % 60);
    return buffer;
}

bool isValidOffset(int offsetMinutes)
{
    return offsetMinutes >= MinOffsetMinutes &&
           offsetMinutes <= MaxOffsetMinutes && offsetMinutes % 15 == 0;
}

int zoneIndex(int offsetMinutes)
{
    const std::vector<Zone> &all = zones();
    for (size_t index = 0; index < all.size(); ++index) {
        if (all[index].offsetMinutes == offsetMinutes) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

bool guessOffset(int64_t consoleUnixSeconds, int64_t utcUnixSeconds,
                 int &offsetMinutes)
{
    const int64_t difference = consoleUnixSeconds - utcUnixSeconds;
    // Round to the nearest quarter hour; real offsets are multiples of 15 min.
    const int64_t quarter = 15 * 60;
    const int64_t rounded =
        (difference >= 0 ? difference + quarter / 2
                         : difference - quarter / 2) / quarter * quarter;
    // A clock that is right except for the zone is within a few minutes of
    // that quarter hour; anything else means the clock itself is wrong.
    if (std::llabs(difference - rounded) > 5 * 60) {
        return false;
    }
    const int minutes = static_cast<int>(rounded / 60);
    if (zoneIndex(minutes) < 0) {
        return false;
    }
    offsetMinutes = minutes;
    return true;
}

std::string describeDrift(int64_t consoleUnixSeconds,
                          int64_t correctUnixSeconds)
{
    const int64_t difference = consoleUnixSeconds - correctUnixSeconds;
    const int64_t magnitude = std::llabs(difference);
    const char *direction = difference < 0 ? "落后" : "超前";

    if (magnitude < 60) {
        return "误差在一分钟以内";
    }
    char buffer[64];
    if (magnitude < 3600) {
        std::snprintf(buffer, sizeof(buffer), "%s %lld 分钟", direction,
                      static_cast<long long>(magnitude / 60));
    } else if (magnitude < 2 * 86400) {
        std::snprintf(buffer, sizeof(buffer), "%s %lld 小时 %lld 分钟", direction,
                      static_cast<long long>(magnitude / 3600),
                      static_cast<long long>((magnitude / 60) % 60));
    } else {
        std::snprintf(buffer, sizeof(buffer), "%s约 %lld 天", direction,
                      static_cast<long long>((magnitude + 43200) / 86400));
    }
    return buffer;
}

int64_t unixSeconds(int year, int month, int day, int hour, int minute,
                    int second)
{
    // Days from 1970-01-01, counting years from March so that the leap day
    // comes last (Howard Hinnant's days_from_civil).
    const int64_t y = year - (month <= 2 ? 1 : 0);
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yearOfEra = y - era * 400;
    const int64_t dayOfYear =
        (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
    const int64_t dayOfEra =
        yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
    const int64_t days = era * 146097 + dayOfEra - 719468;
    return days * 86400 + hour * 3600 + minute * 60 + second;
}

bool coversConsoleTime(int64_t notBefore, int64_t notAfter,
                       int64_t consoleUnixSeconds)
{
    const int64_t earliest = consoleUnixSeconds - MaxOffsetMinutes * 60LL;
    const int64_t latest = consoleUnixSeconds - MinOffsetMinutes * 60LL;
    return notBefore <= notAfter && notBefore <= latest &&
           notAfter >= earliest;
}

} // namespace tz
