// Auto time zone from position: a coarse table of coastal/sailing regions
// (lat/lon boxes, first match wins) with POSIX TZ rules including daylight
// saving. Outside all boxes the nautical time zone (15 deg bands, no DST) is
// used. Boundaries are approximate near borders; the panel always has a manual
// time zone override in Settings > Timezone.

#include "day_night.hpp"
#include <stdio.h>
#include <math.h>

struct TzBox {
    float lat0, lat1, lon0, lon1;
    const char *name;
    const char *posix;
};

#define NZ_TZ   "NZST-12NZDT,M9.5.0,M4.1.0/3"
#define AUE_TZ  "AEST-10AEDT,M10.1.0,M4.1.0/3"
#define EU_CET  "CET-1CEST,M3.5.0,M10.5.0/3"
#define EU_EET  "EET-2EEST,M3.5.0/3,M10.5.0/4"
#define EU_WET  "WET0WEST,M3.5.0/1,M10.5.0"
#define UK_TZ   "GMT0BST,M3.5.0/1,M10.5.0"
#define US_E    "EST5EDT,M3.2.0,M11.1.0"
#define US_C    "CST6CDT,M3.2.0,M11.1.0"
#define US_M    "MST7MDT,M3.2.0,M11.1.0"
#define US_P    "PST8PDT,M3.2.0,M11.1.0"
#define CA_A    "AST4ADT,M3.2.0,M11.1.0"

static const TzBox s_boxes[] = {
    // --- New Zealand and SW Pacific ---
    { -44.6f, -43.4f, -177.2f, -175.8f, "Chatham Islands", "<+1245>-12:45<+1345>,M9.5.0/2:45,M4.1.0/3:45" },
    { -29.5f, -28.5f,  167.5f,  168.5f, "Norfolk Island",  "<+11>-11<+12>,M10.1.0,M4.1.0/3" },
    { -53.0f, -28.0f,  165.5f,  180.0f, "New Zealand",     NZ_TZ },
    { -32.0f, -28.5f, -180.0f, -177.0f, "New Zealand",     NZ_TZ },     // Kermadec Is.
    { -32.0f, -31.0f,  158.8f,  159.4f, "Lord Howe Island","<+1030>-10:30<+11>-11,M10.1.0,M4.1.0" },
    { -23.0f, -19.0f,  163.0f,  169.0f, "New Caledonia",   "<+11>-11" },
    { -21.0f, -13.0f,  166.0f,  171.0f, "Vanuatu",         "<+11>-11" },
    { -12.5f,  -5.0f,  155.0f,  170.0f, "Solomon Islands", "<+11>-11" },
    { -21.5f, -12.0f,  176.0f,  180.0f, "Fiji",            "<+12>-12" },
    { -21.5f, -12.0f, -180.0f, -178.0f, "Fiji",            "<+12>-12" },
    { -23.5f, -15.0f, -177.0f, -173.0f, "Tonga",           "<+13>-13" },
    { -14.5f, -13.0f, -173.0f, -171.2f, "Samoa",           "<+13>-13" },
    { -14.6f, -11.0f, -171.2f, -168.0f, "American Samoa",  "SST11" },
    { -19.5f, -18.5f, -170.5f, -169.5f, "Niue",            "<-11>11" },
    { -22.5f,  -8.0f, -166.5f, -157.0f, "Cook Islands",    "<-10>10" },
    { -28.0f,  -7.0f, -155.0f, -134.0f, "French Polynesia","<-10>10" },
    // --- Australia (QLD before NSW; NSW/VIC/TAS share rules) ---
    { -28.17f, -9.0f,  138.0f,  154.0f, "Queensland",      "AEST-10" },
    { -29.0f,  -9.0f,  138.0f,  152.0f, "Queensland",      "AEST-10" },
    { -26.0f, -10.0f,  129.0f,  138.0f, "Northern Territory", "ACST-9:30" },
    { -38.5f, -26.0f,  129.0f,  141.0f, "South Australia", "ACST-9:30ACDT,M10.1.0,M4.1.0/3" },
    { -36.0f, -13.0f,  112.0f,  129.0f, "Western Australia","AWST-8" },
    { -44.5f, -28.0f,  141.0f,  155.0f, "Australia Eastern", AUE_TZ },
    { -12.0f,   0.0f,  141.0f,  156.0f, "Papua New Guinea","<+10>-10" },
    // --- Asia ---
    {  24.0f,  46.0f,  128.5f,  146.0f, "Japan",           "JST-9" },
    {  33.0f,  39.0f,  124.0f,  131.0f, "Korea",           "KST-9" },
    {  21.5f,  26.5f,  119.5f,  122.5f, "Taiwan",          "CST-8" },
    {   4.5f,  21.5f,  116.0f,  127.0f, "Philippines",     "PST-8" },
    { -11.0f,   4.5f,  127.0f,  141.0f, "Indonesia East",  "WIT-9" },
    { -11.0f,   7.5f,  114.0f,  127.0f, "Indonesia Central","WITA-8" },
    {   0.5f,   7.5f,   99.5f,  119.5f, "Malaysia/Singapore","<+08>-8" },
    { -11.0f,   6.0f,   95.0f,  114.0f, "Indonesia West",  "WIB-7" },
    {   5.5f,  23.5f,   97.0f,  110.0f, "Thailand/Vietnam","<+07>-7" },
    {  18.0f,  54.0f,  108.0f,  135.0f, "China",           "CST-8" },
    {   5.5f,  36.0f,   68.0f,   97.0f, "India",           "IST-5:30" },
    {   5.5f,  10.0f,   79.0f,   82.0f, "Sri Lanka",       "<+0530>-5:30" },
    {  -1.0f,   8.0f,   72.0f,   74.0f, "Maldives",        "<+05>-5" },
    {  22.0f,  27.0f,   51.0f,   57.0f, "UAE/Oman",        "<+04>-4" },
    // --- Atlantic islands ---
    {  36.5f,  40.0f,  -32.0f,  -24.0f, "Azores",          "<-01>1<+00>,M3.5.0/0,M10.5.0/1" },
    {  32.3f,  33.3f,  -17.5f,  -16.0f, "Madeira",         EU_WET },
    {  27.5f,  29.5f,  -18.5f,  -13.0f, "Canary Islands",  EU_WET },
    {  14.5f,  17.5f,  -25.5f,  -22.5f, "Cape Verde",      "<-01>1" },
    {  32.0f,  32.6f,  -65.0f,  -64.5f, "Bermuda",         CA_A },
    {  63.0f,  67.0f,  -25.0f,  -13.0f, "Iceland",         "GMT0" },
    // --- Europe (specific regions before the CET catch-all) ---
    {  49.15f, 49.75f,  -2.75f,  -2.0f, "Channel Islands", UK_TZ },
    {  36.8f,  42.15f, -10.0f,   -7.3f, "Portugal",        EU_WET },
    {  54.3f,  55.3f,   19.6f,   22.9f, "Kaliningrad",     "EET-2" },
    {  53.9f,  59.8f,   20.9f,   28.3f, "Baltic States",   EU_EET },
    {  59.5f,  61.3f,   19.4f,   27.9f, "Finland",         EU_EET },
    {  61.3f,  65.7f,   21.0f,   31.6f, "Finland",         EU_EET },
    {  65.7f,  70.1f,   24.2f,   29.5f, "Finland",         EU_EET },
    {  59.6f,  61.0f,   27.9f,   31.0f, "Russia (Moscow)", "MSK-3" },
    {  34.5f,  35.8f,   32.0f,   34.7f, "Cyprus",          EU_EET },
    {  34.5f,  41.8f,   19.3f,   28.3f, "Greece",          EU_EET },
    {  35.8f,  42.2f,   28.3f,   45.0f, "Turkey",          "<+03>-3" },
    {  41.0f,  48.0f,   27.0f,   40.0f, "Black Sea",       EU_EET },
    {  51.3f,  61.0f,  -11.0f,    1.8f, "UK / Ireland",    UK_TZ },
    {  49.8f,  51.3f,  -11.0f,    1.45f,"UK / Ireland",    UK_TZ },
    {  35.5f,  71.5f,  -9.5f,    24.2f, "Central Europe",  EU_CET },
    // --- Africa ---
    { -35.0f, -22.0f,   16.0f,   33.0f, "South Africa",    "SAST-2" },
    // --- Americas ---
    {  18.0f,  23.0f, -161.0f, -154.0f, "Hawaii",          "HST10" },
    {  54.6f,  72.0f, -180.0f, -129.9f, "Alaska",          "AKST9AKDT,M3.2.0,M11.1.0" },
    {  50.5f,  54.6f, -180.0f, -155.0f, "Alaska (Aleutians)", "AKST9AKDT,M3.2.0,M11.1.0" },
    {  31.3f,  37.0f, -114.8f, -109.0f, "Arizona",         "MST7" },
    {  14.0f,  28.0f, -118.0f, -105.5f, "Mexico Pacific",  "MST7" },
    {  28.0f,  60.0f, -139.0f, -114.5f, "US/Canada Pacific", US_P },
    {  31.0f,  60.0f, -114.5f, -102.0f, "US/Canada Mountain", US_M },
    {  14.0f,  25.8f, -105.5f,  -86.7f, "Mexico",          "CST6" },
    {  46.5f,  52.0f,  -59.7f,  -52.0f, "Newfoundland",    "NST3:30NDT,M3.2.0/0:01,M11.1.0/0:01" },
    {  43.0f,  52.0f,  -67.0f,  -59.7f, "Atlantic Canada", CA_A },
    {  20.9f,  27.3f,  -79.5f,  -72.7f, "Bahamas",         US_E },
    {  10.0f,  19.0f,  -72.0f,  -59.0f, "Caribbean",       "AST4" },
    {  25.0f,  60.0f, -102.0f,  -85.0f, "US/Canada Central", US_C },
    {  24.0f,  57.0f,  -85.0f,  -67.0f, "US/Canada Eastern", US_E },
    { -34.0f,   5.0f,  -52.0f,  -34.0f, "Brazil",          "<-03>3" },
    { -56.0f, -17.0f,  -76.0f,  -69.5f, "Chile",           "<-04>4<-03>,M9.1.6/24,M4.1.6/24" },
    { -56.0f, -21.0f,  -69.5f,  -53.0f, "Argentina",       "<-03>3" },
};

const char *day_night_tz_lookup(double lat, double lon, char *posix, size_t posix_len)
{
    for (const auto &b : s_boxes) {
        if (lat >= b.lat0 && lat <= b.lat1 && lon >= b.lon0 && lon <= b.lon1) {
            snprintf(posix, posix_len, "%s", b.posix);
            return b.name;
        }
    }
    // Nautical time zone: UTC offset = round(lon / 15), no daylight saving
    int z = (int)lround(lon / 15.0);
    if (z > 12) z = 12;
    if (z < -12) z = -12;
    if (z == 0) snprintf(posix, posix_len, "UTC0");
    else        snprintf(posix, posix_len, "<%+03d>%d", z, -z);
    return "Nautical zone";
}
