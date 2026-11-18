#pragma once

#include <limits>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

// 型を信じない JSON の取り出し（どの層からも使える小さな部品）。
//
// ブラウザの /input は誰でも叩けるので、文字列などを混ぜたリクエストで
// nlohmann の value() が type_error を投げ、処理スレッドごと落ちる - それを
// 既定値へ落として続行するための口。シーン文書・UI の文書・エディタの
// コマンドが共用する唯一の定義。
namespace wizengine {

// 数値（型が違えば fallback）。
inline double jsonNumber(const nlohmann::json& j, const char* key,
                         double fallback) {
    if (!j.is_object()) return fallback;
    const auto it = j.find(key);
    return (it != j.end() && it->is_number()) ? it->get<double>() : fallback;
}
// int に入らない値（NaN・無限・範囲外）も fallback（double から int への変換は
// 範囲外で未定義）。小数は 0 の向きへ切り捨てる。
inline int jsonInt(const nlohmann::json& j, const char* key, int fallback) {
    if (!j.is_object()) return fallback;
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number()) return fallback;
    const double v = it->get<double>();
    // NaN はどの比較も偽 = fallback。
    if (!(v > double(std::numeric_limits<int>::min()) - 1.0 &&
          v < double(std::numeric_limits<int>::max()) + 1.0))
        return fallback;
    return int(v);
}
// 文字列版（型が違えば fallback。nlohmann の value() のように投げない）。
inline std::string jsonString(const nlohmann::json& j, const char* key,
                              const std::string& fallback) {
    if (!j.is_object()) return fallback;
    const auto it = j.find(key);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : fallback;
}
// 文字列の配列（イベントアセット名の一覧など）。文字列以外の要素は捨てる
// - 型を信じない、の同じ流儀。
inline std::vector<std::string> stringList(const nlohmann::json& j) {
    std::vector<std::string> out;
    if (!j.is_array()) return out;
    for (const auto& e : j) {
        if (e.is_string()) out.push_back(e.get<std::string>());
    }
    return out;
}

}  // namespace wizengine
