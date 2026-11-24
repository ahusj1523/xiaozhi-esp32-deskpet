#include "weather_city_store.h"

#include "settings.h"

#include <cstdio>
#include <cstdlib>

namespace {
constexpr const char* kNamespace = "weather";
constexpr const char* kKeyCity = "manual_city";
constexpr const char* kKeyLat = "manual_lat";
constexpr const char* kKeyLon = "manual_lon";
// 默认城市：江西省吉安市永新县（家家旺超市所在地）。
// 没有手工选过城市时直接用它，省掉 GeoIP 定位那一步（那一步最容易失败）。
constexpr const char* kDefaultCity = "永新县";
constexpr double kDefaultLat = 26.9360;
constexpr double kDefaultLon = 114.2350;
}  // namespace

CityLocation WeatherCityStore::GetCity() const {
    Settings settings(kNamespace, false);
    std::string city = settings.GetString(kKeyCity);
    std::string lat_s = settings.GetString(kKeyLat);
    std::string lon_s = settings.GetString(kKeyLon);

    CityLocation location;
    if (!city.empty() && !lat_s.empty() && !lon_s.empty()) {
        location.found = true;
        location.city = city;
        location.lat = atof(lat_s.c_str());
        location.lon = atof(lon_s.c_str());
    } else {
        location.found = true;
        location.city = kDefaultCity;
        location.lat = kDefaultLat;
        location.lon = kDefaultLon;
    }
    return location;
}

void WeatherCityStore::SetCity(const std::string& city, double lat, double lon) {
    char coord[24];
    Settings settings(kNamespace, true);
    settings.SetString(kKeyCity, city);
    snprintf(coord, sizeof(coord), "%.4f", lat);
    settings.SetString(kKeyLat, coord);
    snprintf(coord, sizeof(coord), "%.4f", lon);
    settings.SetString(kKeyLon, coord);
}

void WeatherCityStore::Clear() {
    Settings settings(kNamespace, true);
    settings.EraseKey(kKeyCity);
    settings.EraseKey(kKeyLat);
    settings.EraseKey(kKeyLon);
}
