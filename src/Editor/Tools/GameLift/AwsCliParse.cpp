//====================================================================================
//                          AwsCliParse.cpp
//  MyEngin/ 秋田蓮音                                                       11/27/2026
//                                          aws CLI の応答 (JSON / テキスト) の解析とエラー分類の実装
//====================================================================================
#include "Editor/Tools/GameLift/AwsCliParse.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstring>

#include <Windows.h>

#include "nlohmann/json.hpp"

namespace mye {
namespace {

using Json = nlohmann::json;

std::string Trim(const std::string& text)
{
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return text.substr(begin, end - begin);
}

// 空行を除いた行の列 (CR / LF どちらの区切りも受ける)
std::vector<std::string> NonEmptyLines(const std::string& text, bool splitOnCarriageReturn)
{
    std::vector<std::string> lines;
    std::string current;
    const auto flush = [&]() {
        const std::string trimmed = Trim(current);
        if (!trimmed.empty()) {
            lines.push_back(trimmed);
        }
        current.clear();
    };
    for (const char c : text) {
        if (c == '\n' || (splitOnCarriageReturn && c == '\r')) {
            flush();
        } else {
            current.push_back(c);
        }
    }
    flush();
    return lines;
}

bool ContainsNoCase(const std::string& haystack, const char* needle)
{
    const size_t needleLength = std::strlen(needle);
    if (needleLength > haystack.size()) {
        return false;
    }
    for (size_t i = 0; i + needleLength <= haystack.size(); ++i) {
        size_t k = 0;
        while (k < needleLength && std::tolower(static_cast<unsigned char>(haystack[i + k])) ==
                                       std::tolower(static_cast<unsigned char>(needle[k]))) {
            ++k;
        }
        if (k == needleLength) {
            return true;
        }
    }
    return false;
}

AwsErrorKind KindFromCode(const std::string& code)
{
    static const char* const kCredentialCodes[] = {
        "ExpiredToken",      "ExpiredTokenException", "UnrecognizedClientException", "InvalidClientTokenId",
        "InvalidAccessKeyId", "AuthFailure",          "RequestExpired",              "InvalidSignatureException",
        "SignatureDoesNotMatch", "TokenRefreshRequired",
    };
    for (const char* known : kCredentialCodes) {
        if (code == known) {
            return AwsErrorKind::Credentials;
        }
    }
    if (code == "NotFoundException") {
        return AwsErrorKind::NotFound;
    }
    if (code == "FleetCapacityExceededException") {
        return AwsErrorKind::CapacityExceeded;
    }
    if (code == "ConflictException" || code == "UniqueIdentifierConflictException") {
        return AwsErrorKind::Conflict;
    }
    if (code == "InvalidRequestException") {
        return AwsErrorKind::InvalidRequest;
    }
    return AwsErrorKind::Other;
}

bool LooksLikeMissingCredentials(const std::string& text)
{
    static const char* const kPhrases[] = {
        "Unable to locate credentials", "NoCredentialsError", "Your session has expired", "Error loading SSO Token",
        "Token has expired",            "aws login",          "aws sso login",
    };
    for (const char* phrase : kPhrases) {
        if (ContainsNoCase(text, phrase)) {
            return true;
        }
    }
    return false;
}

// "An error occurred (Code) when calling the Op operation[ (reached max retries: 4)]: message"
bool FindTypedError(const std::string& text, AwsError& out)
{
    static const char kHead[] = "An error occurred (";
    const size_t head = text.find(kHead);
    if (head == std::string::npos) {
        return false;
    }
    const size_t codeBegin = head + sizeof(kHead) - 1;
    const size_t codeEnd = text.find(')', codeBegin);
    if (codeEnd == std::string::npos) {
        return false;
    }
    out.code = text.substr(codeBegin, codeEnd - codeBegin);

    static const char kCalling[] = " when calling the ";
    size_t pos = codeEnd + 1;
    if (text.compare(pos, sizeof(kCalling) - 1, kCalling) == 0) {
        pos += sizeof(kCalling) - 1;
        const size_t opEnd = text.find(" operation", pos);
        if (opEnd != std::string::npos) {
            out.operation = text.substr(pos, opEnd - pos);
            pos = opEnd + sizeof(" operation") - 1;
        }
    }
    if (pos < text.size() && text.compare(pos, 2, " (") == 0) {
        const size_t close = text.find(')', pos);
        if (close != std::string::npos) {
            pos = close + 1;
        }
    }
    if (pos < text.size() && text[pos] == ':') {
        ++pos;
    }
    const std::vector<std::string> lines = NonEmptyLines(text.substr(pos), false);
    out.message = lines.empty() ? std::string() : lines.front();
    return true;
}

// --- JSON の取り出し (型が違っても例外を出さず既定値) ---

std::string Str(const Json& object, const char* key)
{
    const auto it = object.find(key);
    if (it == object.end()) {
        return std::string();
    }
    if (it->is_string()) {
        return it->get<std::string>();
    }
    if (it->is_number()) {
        return it->dump();
    }
    return std::string();
}

int Int(const Json& object, const char* key)
{
    const auto it = object.find(key);
    if (it == object.end() || !it->is_number()) {
        return 0;
    }
    return it->is_number_integer() ? static_cast<int>(it->get<long long>()) : static_cast<int>(it->get<double>());
}

bool ParseRoot(const std::string& text, Json& root, std::string& error)
{
    root = Json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        error = "the response is not a JSON object";
        return false;
    }
    return true;
}

// root[key] がオブジェクトなら返す
bool RequireObject(const Json& root, const char* key, const Json*& out, std::string& error)
{
    const auto it = root.find(key);
    if (it == root.end() || !it->is_object()) {
        error = std::string("missing object \"") + key + "\"";
        return false;
    }
    out = &*it;
    return true;
}

// root[key] が配列なら返す (キー自体が無いのは空として許す = CLI は空結果でキーを省くことがある)
bool OptionalArray(const Json& root, const char* key, const Json*& out, std::string& error)
{
    out = nullptr;
    const auto it = root.find(key);
    if (it == root.end()) {
        return true;
    }
    if (!it->is_array()) {
        error = std::string("\"") + key + "\" is not an array";
        return false;
    }
    out = &*it;
    return true;
}

bool RequireField(const std::string& value, const char* what, std::string& error)
{
    if (value.empty()) {
        error = std::string("missing ") + what;
        return false;
    }
    return true;
}

AwsLocation ReadLocation(const Json& object)
{
    return { Str(object, "LocationName"), Str(object, "LocationArn") };
}

AwsFleetAttributes ReadFleetAttributes(const Json& object)
{
    AwsFleetAttributes fleet;
    fleet.fleetId = Str(object, "FleetId");
    fleet.fleetArn = Str(object, "FleetArn");
    fleet.name = Str(object, "Name");
    fleet.status = Str(object, "Status");
    fleet.computeType = Str(object, "ComputeType");
    fleet.instanceType = Str(object, "InstanceType");
    fleet.buildId = Str(object, "BuildId");
    return fleet;
}

AwsGameSession ReadGameSession(const Json& object)
{
    AwsGameSession session;
    session.gameSessionId = Str(object, "GameSessionId");
    session.fleetId = Str(object, "FleetId");
    session.status = Str(object, "Status");
    session.statusReason = Str(object, "StatusReason");
    session.location = Str(object, "Location");
    session.ipAddress = Str(object, "IpAddress");
    session.dnsName = Str(object, "DnsName");
    session.port = Int(object, "Port");
    session.currentPlayers = Int(object, "CurrentPlayerSessionCount");
    session.maxPlayers = Int(object, "MaximumPlayerSessionCount");
    return session;
}

} // namespace

AwsError ClassifyAwsFailure(int exitCode, const std::string& stderrText)
{
    AwsError error;
    if (exitCode == 0) {
        return error;
    }
    if (FindTypedError(stderrText, error)) {
        error.kind = KindFromCode(error.code);
        return error;
    }
    const std::vector<std::string> lines = NonEmptyLines(stderrText, false);
    error.message = lines.empty() ? std::string("aws exited with code ") + std::to_string(exitCode) : lines.front();
    if (LooksLikeMissingCredentials(stderrText)) {
        error.kind = AwsErrorKind::Credentials;
    } else if (ContainsNoCase(stderrText, "You must specify a region")) {
        error.kind = AwsErrorKind::NoRegion;
    } else {
        error.kind = AwsErrorKind::Other;
    }
    return error;
}

std::string FirstErrorLines(const std::string& stderrText, int maxLines)
{
    std::string joined;
    int taken = 0;
    for (const std::string& line : NonEmptyLines(stderrText, false)) {
        if (taken >= maxLines) {
            break;
        }
        if (taken > 0) {
            joined += '\n';
        }
        joined += line;
        ++taken;
    }
    return joined;
}

bool ParseAwsCliVersion(const std::string& text, AwsCliVersion& out)
{
    static const char kTag[] = "aws-cli/";
    for (const std::string& line : NonEmptyLines(text, false)) {
        const size_t tag = line.find(kTag);
        if (tag == std::string::npos) {
            continue;
        }
        const char* cursor = line.c_str() + tag + sizeof(kTag) - 1;
        int numbers[3] = { 0, 0, 0 };
        for (int i = 0; i < 3; ++i) {
            if (!std::isdigit(static_cast<unsigned char>(*cursor))) {
                return false;
            }
            char* end = nullptr;
            numbers[i] = static_cast<int>(std::strtol(cursor, &end, 10));
            cursor = end;
            if (i < 2) {
                if (*cursor != '.') {
                    return false;
                }
                ++cursor;
            }
        }
        out.major = numbers[0];
        out.minor = numbers[1];
        out.patch = numbers[2];
        out.text = line;
        return true;
    }
    return false;
}

bool ParseSingleLineText(const std::string& text, std::string& out)
{
    const std::vector<std::string> lines = NonEmptyLines(text, false);
    if (lines.empty()) {
        return false;
    }
    out = lines.front();
    return true;
}

AwsUploadProgress ParseUploadBuildOutput(const std::string& text)
{
    static const char kBuildIdTag[] = "Build ID:";
    AwsUploadProgress progress;
    for (const std::string& segment : NonEmptyLines(text, true)) {
        const size_t tag = segment.find(kBuildIdTag);
        if (tag != std::string::npos) {
            progress.buildId = Trim(segment.substr(tag + sizeof(kBuildIdTag) - 1));
            continue;
        }
        // 行末側の "NN.NN%" を読む ("Uploading ... (21.45%)")
        const size_t percentSign = segment.rfind('%');
        if (percentSign == std::string::npos) {
            continue;
        }
        size_t begin = percentSign;
        while (begin > 0 && (std::isdigit(static_cast<unsigned char>(segment[begin - 1])) || segment[begin - 1] == '.')) {
            --begin;
        }
        double value = 0.0;
        const char* first = segment.data() + begin;
        const char* last = segment.data() + percentSign;
        const auto result = std::from_chars(first, last, value);
        if (begin < percentSign && result.ec == std::errc() && result.ptr == last) {
            progress.percent = value;
        }
    }
    return progress;
}

bool ParseCallerIdentity(const std::string& json, AwsCallerIdentity& out, std::string& error)
{
    Json root;
    if (!ParseRoot(json, root, error)) {
        return false;
    }
    out.account = Str(root, "Account");
    out.arn = Str(root, "Arn");
    out.userId = Str(root, "UserId");
    return RequireField(out.account, "Account", error);
}

bool ParseListLocations(const std::string& json, std::vector<AwsLocation>& out, std::string& error)
{
    Json root;
    if (!ParseRoot(json, root, error)) {
        return false;
    }
    const Json* array = nullptr;
    if (!OptionalArray(root, "Locations", array, error)) {
        return false;
    }
    out.clear();
    if (array != nullptr) {
        for (const Json& item : *array) {
            if (item.is_object()) {
                out.push_back(ReadLocation(item));
            }
        }
    }
    return true;
}

bool ParseCreateLocation(const std::string& json, AwsLocation& out, std::string& error)
{
    Json root;
    const Json* location = nullptr;
    if (!ParseRoot(json, root, error) || !RequireObject(root, "Location", location, error)) {
        return false;
    }
    out = ReadLocation(*location);
    return RequireField(out.name, "LocationName", error);
}

bool ParseCreateFleet(const std::string& json, AwsFleetAttributes& out, std::string& error)
{
    Json root;
    const Json* fleet = nullptr;
    if (!ParseRoot(json, root, error) || !RequireObject(root, "FleetAttributes", fleet, error)) {
        return false;
    }
    out = ReadFleetAttributes(*fleet);
    return RequireField(out.fleetId, "FleetId", error);
}

bool ParseDescribeFleetAttributes(const std::string& json, std::vector<AwsFleetAttributes>& out, std::string& error)
{
    Json root;
    const Json* array = nullptr;
    if (!ParseRoot(json, root, error) || !OptionalArray(root, "FleetAttributes", array, error)) {
        return false;
    }
    out.clear();
    if (array != nullptr) {
        for (const Json& item : *array) {
            if (item.is_object()) {
                out.push_back(ReadFleetAttributes(item));
            }
        }
    }
    return true;
}

bool ParseDescribeFleetCapacity(const std::string& json, std::vector<AwsFleetCapacity>& out, std::string& error)
{
    Json root;
    const Json* array = nullptr;
    if (!ParseRoot(json, root, error) || !OptionalArray(root, "FleetCapacity", array, error)) {
        return false;
    }
    out.clear();
    if (array == nullptr) {
        return true;
    }
    for (const Json& item : *array) {
        if (!item.is_object()) {
            continue;
        }
        AwsFleetCapacity capacity;
        capacity.fleetId = Str(item, "FleetId");
        capacity.instanceType = Str(item, "InstanceType");
        capacity.location = Str(item, "Location");
        const auto counts = item.find("InstanceCounts");
        if (counts != item.end() && counts->is_object()) {
            capacity.desired = Int(*counts, "DESIRED");
            capacity.minimum = Int(*counts, "MINIMUM");
            capacity.maximum = Int(*counts, "MAXIMUM");
            capacity.pending = Int(*counts, "PENDING");
            capacity.active = Int(*counts, "ACTIVE");
            capacity.idle = Int(*counts, "IDLE");
            capacity.terminating = Int(*counts, "TERMINATING");
        }
        out.push_back(capacity);
    }
    return true;
}

bool ParseDescribeFleetEvents(const std::string& json, std::vector<AwsFleetEvent>& out, std::string& error)
{
    Json root;
    const Json* array = nullptr;
    if (!ParseRoot(json, root, error) || !OptionalArray(root, "Events", array, error)) {
        return false;
    }
    out.clear();
    if (array != nullptr) {
        for (const Json& item : *array) {
            if (item.is_object()) {
                out.push_back({ Str(item, "EventCode"), Str(item, "Message"), Str(item, "EventTime") });
            }
        }
    }
    return true;
}

bool ParseCompute(const std::string& json, AwsCompute& out, std::string& error)
{
    Json root;
    const Json* compute = nullptr;
    if (!ParseRoot(json, root, error) || !RequireObject(root, "Compute", compute, error)) {
        return false;
    }
    out.fleetId = Str(*compute, "FleetId");
    out.computeName = Str(*compute, "ComputeName");
    out.computeArn = Str(*compute, "ComputeArn");
    out.status = Str(*compute, "ComputeStatus");
    out.location = Str(*compute, "Location");
    out.ipAddress = Str(*compute, "IpAddress");
    out.dnsName = Str(*compute, "DnsName");
    out.sdkEndpoint = Str(*compute, "GameLiftServiceSdkEndpoint");
    return RequireField(out.computeName, "ComputeName", error);
}

void AwsComputeAuthToken::Wipe()
{
    if (!authToken.empty()) {
        SecureZeroMemory(authToken.data(), authToken.size());
    }
    authToken.clear();
}

bool ParseComputeAuthToken(const std::string& json, AwsComputeAuthToken& out, std::string& error)
{
    Json root;
    if (!ParseRoot(json, root, error)) {
        return false;
    }
    out.computeName = Str(root, "ComputeName");
    out.authToken = Str(root, "AuthToken");
    out.expiration = Str(root, "ExpirationTimeStamp");
    return RequireField(out.authToken, "AuthToken", error);
}

bool ParseCreateGameSession(const std::string& json, AwsGameSession& out, std::string& error)
{
    Json root;
    const Json* session = nullptr;
    if (!ParseRoot(json, root, error) || !RequireObject(root, "GameSession", session, error)) {
        return false;
    }
    out = ReadGameSession(*session);
    return RequireField(out.gameSessionId, "GameSessionId", error);
}

bool ParseDescribeGameSessions(const std::string& json, std::vector<AwsGameSession>& out, std::string& error)
{
    Json root;
    const Json* array = nullptr;
    if (!ParseRoot(json, root, error) || !OptionalArray(root, "GameSessions", array, error)) {
        return false;
    }
    out.clear();
    if (array != nullptr) {
        for (const Json& item : *array) {
            if (item.is_object()) {
                out.push_back(ReadGameSession(item));
            }
        }
    }
    return true;
}

bool ParseCreatePlayerSession(const std::string& json, AwsPlayerSession& out, std::string& error)
{
    Json root;
    const Json* session = nullptr;
    if (!ParseRoot(json, root, error) || !RequireObject(root, "PlayerSession", session, error)) {
        return false;
    }
    out.playerSessionId = Str(*session, "PlayerSessionId");
    out.playerId = Str(*session, "PlayerId");
    out.gameSessionId = Str(*session, "GameSessionId");
    out.status = Str(*session, "Status");
    out.ipAddress = Str(*session, "IpAddress");
    out.dnsName = Str(*session, "DnsName");
    out.port = Int(*session, "Port");
    return RequireField(out.playerSessionId, "PlayerSessionId", error);
}

bool ParseDescribeBuild(const std::string& json, AwsBuild& out, std::string& error)
{
    Json root;
    const Json* build = nullptr;
    if (!ParseRoot(json, root, error) || !RequireObject(root, "Build", build, error)) {
        return false;
    }
    out.buildId = Str(*build, "BuildId");
    out.name = Str(*build, "Name");
    out.version = Str(*build, "Version");
    out.status = Str(*build, "Status");
    out.operatingSystem = Str(*build, "OperatingSystem");
    return RequireField(out.buildId, "BuildId", error);
}

} // namespace mye
