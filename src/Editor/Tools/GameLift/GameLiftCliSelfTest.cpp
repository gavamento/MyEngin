//====================================================================================
//                          GameLiftCliSelfTest.cpp
//  MyEngin/ 秋田蓮音                                                       11/27/2026
//                                          aws CLI 層 (argv・解析・子プロセス実行器) の回帰テスト実装
//====================================================================================
#include "Editor/Tools/GameLift/GameLiftCliSelfTest.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <Windows.h>
#include <shellapi.h>

#include "Editor/Tools/GameLift/AwsCliCommands.h"
#include "Editor/Tools/GameLift/AwsCliParse.h"
#include "Editor/Tools/GameLift/CliProcess.h"
#include "Engine/Core/Diagnostics/Log.h"

namespace fs = std::filesystem;

namespace mye {
namespace {

struct Counter
{
    int failCount = 0;
    void Check(bool cond, const char* what)
    {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    }
};

using Args = std::vector<std::wstring>;

// ---------------------------------------------------------------------------------
// 純関数
// ---------------------------------------------------------------------------------

void TestQuotingRoundTrip(Counter& c)
{
    const std::wstring exe = L"C:\\Program Files\\Amazon\\AWSCLIV2\\aws.exe";
    const Args args = {
        L"",
        L"plain",
        L"a b",
        L"say \"hi\"",
        L"trail\\",
        L"trail dir\\",
        L"a\\\"b",
        L"\\\\server\\share",
        L"\\\\\"",
        L"\"",
        L"tab\there",
        L"日本語 パス\\",
        L"file://C:\\a b\\c.json",
        L"--region",
    };
    const std::wstring line = BuildCommandLine(exe, args);
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(line.c_str(), &argc);
    bool same = argv != nullptr && static_cast<size_t>(argc) == args.size() + 1;
    if (same) {
        same = (exe == argv[0]);
        for (size_t i = 0; same && i < args.size(); ++i) {
            same = (args[i] == argv[i + 1]);
        }
    }
    if (argv != nullptr) {
        LocalFree(argv);
    }
    c.Check(same, "BuildCommandLine -> CommandLineToArgvW restores the original array (space / quote / trailing backslash / empty / Japanese)");

    c.Check(BuildCommandLine(L"cmd.exe", { L"/c", L"echo" }) == L"cmd.exe /c echo", "plain arguments are not quoted");
}

void TestValidators(Counter& c)
{
    c.Check(IsValidLocationSuffix("mye-dev") && IsValidLocationSuffix("a.b_c-1"), "location suffix: allowed characters");
    c.Check(!IsValidLocationSuffix("") && !IsValidLocationSuffix("a b") && !IsValidLocationSuffix("a/b") &&
                !IsValidLocationSuffix("custom-x") && !IsValidLocationSuffix("-x") && !IsValidLocationSuffix("あ") &&
                !IsValidLocationSuffix(std::string(51, 'a')),
            "location suffix: empty / space / slash / custom- prefix / leading '-' / non-ASCII / too long are rejected");
    c.Check(IsValidLocationSuffix(std::string(50, 'a')), "location suffix: 50 characters is the limit");
    c.Check(IsValidFleetName("mye-anywhere") && !IsValidFleetName("my fleet") && !IsValidFleetName("--x") &&
                !IsValidFleetName("a\"b") && !IsValidFleetName("a&b"),
            "fleet name");
    c.Check(IsValidComputeName("PC-01") && !IsValidComputeName("pc 01") && !IsValidComputeName(""), "compute name");
    c.Check(IsValidBuildName("mye-server") && IsValidBuildVersion("20261127.1") && !IsValidBuildVersion("1 2"),
            "build name / version");
    c.Check(IsValidInstanceType("c5.large") && !IsValidInstanceType("c5 large") && !IsValidInstanceType("c5;large"),
            "instance type");
    c.Check(IsValidIpv4("127.0.0.1") && IsValidIpv4("255.255.255.255") && IsValidIpv4("0.0.0.0"), "ipv4: valid");
    c.Check(!IsValidIpv4("") && !IsValidIpv4("1.2.3") && !IsValidIpv4("1.2.3.4.5") && !IsValidIpv4("256.1.1.1") &&
                !IsValidIpv4("a.b.c.d") && !IsValidIpv4("1..2.3") && !IsValidIpv4("01.2.3.4") && !IsValidIpv4("1.2.3.4 "),
            "ipv4: invalid");
    c.Check(IsValidPort(1) && IsValidPort(7777) && IsValidPort(65535) && !IsValidPort(0) && !IsValidPort(65536) &&
                !IsValidPort(-1),
            "port 1..65535");
    c.Check(IsValidMaxPlayers(1) && IsValidMaxPlayers(4) && !IsValidMaxPlayers(0) && !IsValidMaxPlayers(5), "players 1..4");
    c.Check(IsValidRegion("ap-northeast-1") && IsValidRegion("us-gov-west-1") && !IsValidRegion("") &&
                !IsValidRegion("AP-NORTHEAST-1") && !IsValidRegion("ap northeast") && !IsValidRegion("--x"),
            "region");
    c.Check(IsValidResourceId("fleet-1234abcd-1234-1234-1234-1234567890ab") &&
                IsValidResourceId("arn:aws:gamelift:ap-northeast-1::gamesession/fleet-x/custom-l/gsess-1") &&
                !IsValidResourceId("--fleet") && !IsValidResourceId("a b"),
            "resource id (ARN allowed, option-like rejected)");
    c.Check(LocationName("mye-dev") == "custom-mye-dev", "location name = custom- + suffix");
}

bool ArgsEqual(const AwsCall& call, const Args& expected)
{
    return call.args == expected;
}

void TestCommands(Counter& c)
{
    const std::string region = "ap-northeast-1";
    const Args tail = { L"--output", L"json", L"--no-cli-pager", L"--region", L"ap-northeast-1" };
    const auto with = [&](Args head) {
        head.insert(head.end(), tail.begin(), tail.end());
        return head;
    };

    AwsCall call;
    c.Check(AwsStsGetCallerIdentityCall("", call) &&
                ArgsEqual(call, { L"sts", L"get-caller-identity", L"--output", L"json", L"--no-cli-pager" }),
            "sts without a region omits --region");
    c.Check(AwsStsGetCallerIdentityCall(region, call) &&
                ArgsEqual(call, { L"sts", L"get-caller-identity", L"--output", L"json", L"--no-cli-pager", L"--region",
                                  L"ap-northeast-1" }),
            "sts with a region appends --region");
    c.Check(!AwsStsGetCallerIdentityCall("Bad Region", call), "sts rejects a bad region");
    c.Check(AwsVersionCall().args == Args{ L"--version" } && AwsConfigureGetRegionCall().args == Args({ L"configure", L"get", L"region" }),
            "--version / configure get region");

    c.Check(GameLiftListCustomLocationsCall(region, call) &&
                ArgsEqual(call, with({ L"gamelift", L"list-locations", L"--filters", L"CUSTOM" })),
            "list-locations --filters CUSTOM");
    c.Check(GameLiftCreateLocationCall(region, "mye-dev", call) &&
                ArgsEqual(call, with({ L"gamelift", L"create-location", L"--location-name", L"custom-mye-dev" })),
            "create-location custom-<suffix>");
    c.Check(GameLiftDeleteLocationCall(region, "mye-dev", call) &&
                ArgsEqual(call, with({ L"gamelift", L"delete-location", L"--location-name", L"custom-mye-dev" })),
            "delete-location");
    c.Check(GameLiftCreateAnywhereFleetCall(region, "mye-anywhere", "mye-dev", call) &&
                ArgsEqual(call, with({ L"gamelift", L"create-fleet", L"--name", L"mye-anywhere", L"--compute-type",
                                       L"ANYWHERE", L"--locations", L"Location=custom-mye-dev" })),
            "create-fleet (ANYWHERE, Location=custom-<x>)");
    c.Check(GameLiftCreateFleetFromJsonCall(region, L"C:\\p q\\create-fleet.json", call) &&
                ArgsEqual(call, with({ L"gamelift", L"create-fleet", L"--cli-input-json",
                                       L"file://C:\\p q\\create-fleet.json" })),
            "create-fleet --cli-input-json file://<path>");
    c.Check(GameLiftDescribeFleetAttributesCall(region, "fleet-1", call) &&
                ArgsEqual(call, with({ L"gamelift", L"describe-fleet-attributes", L"--fleet-ids", L"fleet-1" })),
            "describe-fleet-attributes");
    c.Check(GameLiftDescribeFleetCapacityCall(region, "fleet-1", call) &&
                ArgsEqual(call, with({ L"gamelift", L"describe-fleet-capacity", L"--fleet-ids", L"fleet-1" })),
            "describe-fleet-capacity");
    c.Check(GameLiftDescribeFleetEventsCall(region, "fleet-1", 10, call) &&
                ArgsEqual(call, with({ L"gamelift", L"describe-fleet-events", L"--fleet-id", L"fleet-1", L"--limit",
                                       L"10" })) &&
                !GameLiftDescribeFleetEventsCall(region, "fleet-1", 0, call),
            "describe-fleet-events --limit (0 rejected)");
    c.Check(GameLiftUpdateFleetCapacityCall(region, "fleet-1", 0, call) &&
                ArgsEqual(call, with({ L"gamelift", L"update-fleet-capacity", L"--fleet-id", L"fleet-1",
                                       L"--desired-instances", L"0", L"--min-size", L"0", L"--max-size", L"1" })) &&
                !GameLiftUpdateFleetCapacityCall(region, "fleet-1", 2, call),
            "update-fleet-capacity 0|1 (2 rejected)");
    c.Check(GameLiftDeleteFleetCall(region, "fleet-1", call) &&
                ArgsEqual(call, with({ L"gamelift", L"delete-fleet", L"--fleet-id", L"fleet-1" })),
            "delete-fleet");

    c.Check(GameLiftRegisterComputeCall(region, "fleet-1", "PC-01", "127.0.0.1", "mye-dev", call) &&
                ArgsEqual(call, with({ L"gamelift", L"register-compute", L"--fleet-id", L"fleet-1", L"--compute-name",
                                       L"PC-01", L"--ip-address", L"127.0.0.1", L"--location", L"custom-mye-dev" })),
            "register-compute (ip + location)");
    c.Check(!GameLiftRegisterComputeCall(region, "fleet-1", "PC-01", "127.0.0", "mye-dev", call), "register-compute rejects a bad ip");
    c.Check(GameLiftDescribeComputeCall(region, "fleet-1", "PC-01", call) && !call.secret &&
                ArgsEqual(call, with({ L"gamelift", L"describe-compute", L"--fleet-id", L"fleet-1", L"--compute-name", L"PC-01" })),
            "describe-compute is not secret");
    c.Check(GameLiftGetComputeAuthTokenCall(region, "fleet-1", "PC-01", call) && call.secret &&
                call.label == L"gamelift get-compute-auth-token",
            "get-compute-auth-token is secret and its label has no values");
    c.Check(GameLiftDeregisterComputeCall(region, "fleet-1", "PC-01", call) && call.args[1] == L"deregister-compute",
            "deregister-compute");

    c.Check(GameLiftCreateGameSessionCall(region, "fleet-1", "mye-dev", 4, call) &&
                ArgsEqual(call, with({ L"gamelift", L"create-game-session", L"--fleet-id", L"fleet-1", L"--location",
                                       L"custom-mye-dev", L"--maximum-player-session-count", L"4" })),
            "create-game-session with a location");
    c.Check(GameLiftCreateGameSessionCall(region, "fleet-1", "", 2, call) &&
                ArgsEqual(call, with({ L"gamelift", L"create-game-session", L"--fleet-id", L"fleet-1",
                                       L"--maximum-player-session-count", L"2" })) &&
                !GameLiftCreateGameSessionCall(region, "fleet-1", "", 5, call),
            "create-game-session without a location (EC2), 5 players rejected");
    DescribeGameSessionsQuery query;
    query.fleetId = "fleet-1";
    query.locationSuffix = "mye-dev";
    query.gameSessionId = "gsess-1";
    query.activeOnly = true;
    c.Check(GameLiftDescribeGameSessionsCall(region, query, call) &&
                ArgsEqual(call, with({ L"gamelift", L"describe-game-sessions", L"--fleet-id", L"fleet-1", L"--location",
                                       L"custom-mye-dev", L"--game-session-id", L"gsess-1", L"--status-filter",
                                       L"ACTIVE" })),
            "describe-game-sessions with every filter");
    c.Check(GameLiftCreatePlayerSessionCall(region, "gsess-1", "p-1", call) &&
                ArgsEqual(call, with({ L"gamelift", L"create-player-session", L"--game-session-id", L"gsess-1",
                                       L"--player-id", L"p-1" })),
            "create-player-session");

    c.Check(GameLiftUploadBuildCall(region, "mye-server", "1", L"C:\\p q\\server-build", call) &&
                ArgsEqual(call, with({ L"gamelift", L"upload-build", L"--name", L"mye-server", L"--build-version",
                                       L"1", L"--build-root", L"C:\\p q\\server-build", L"--operating-system",
                                       L"WINDOWS_2022", L"--server-sdk-version", L"5.6.0" })),
            "upload-build (WINDOWS_2022 / 5.6.0 / build-root)");
    c.Check(GameLiftDescribeBuildCall(region, "build-1", call) &&
                ArgsEqual(call, with({ L"gamelift", L"describe-build", L"--build-id", L"build-1" })) &&
                GameLiftDeleteBuildCall(region, "build-1", call) && call.args[1] == L"delete-build",
            "describe-build / delete-build");

    // 不正な入力では out を変えない
    AwsCall untouched;
    untouched.label = L"keep";
    c.Check(!GameLiftCreateLocationCall(region, "custom-x", untouched) && !GameLiftCreateLocationCall(region, "a b", untouched) &&
                !GameLiftCreateLocationCall("", "x", untouched) && !GameLiftDeleteFleetCall(region, "--bad", untouched) &&
                untouched.label == L"keep" && untouched.args.empty(),
            "invalid input returns false and leaves the output untouched");

    // 入力検査を通った値は 1 要素のままで、空白や引用符で argv が割れない
    c.Check(GameLiftCreateAnywhereFleetCall(region, "a\" --evil x", "mye-dev", untouched) == false, "a quote or space cannot smuggle extra options");

    // aws.exe の探索順
    const std::vector<std::wstring> order = AwsExecutableCandidates(L"D:\\tools\\aws.exe", L"C:\\path\\aws.exe");
    c.Check(order.size() == 3 && order[0] == L"D:\\tools\\aws.exe" && order[1] == L"C:\\path\\aws.exe" &&
                order[2] == L"C:\\Program Files\\Amazon\\AWSCLIV2\\aws.exe",
            "aws.exe candidates: configured -> SearchPath -> default install");
    const std::vector<std::wstring> deduped = AwsExecutableCandidates(L"", L"C:\\Program Files\\Amazon\\AWSCLIV2\\aws.exe");
    c.Check(deduped.size() == 1, "aws.exe candidates: empty is skipped and duplicates are merged");
}

// 環境ブロックを "NAME=value" の列へ戻す
std::vector<std::wstring> SplitBlock(const std::wstring& block)
{
    std::vector<std::wstring> entries;
    for (size_t pos = 0; pos < block.size() && block[pos] != L'\0';) {
        const size_t end = block.find(L'\0', pos);
        entries.push_back(block.substr(pos, end - pos));
        pos = end + 1;
    }
    return entries;
}

void TestEnvironmentBlock(Counter& c)
{
    const std::vector<CliEnvVar> parent = { { L"Path", L"a" }, { L"=C:", L"C:\\" }, { L"abc", L"1" } };
    const std::vector<CliEnvVar> overrides = { { L"ABC", L"2" }, { L"AWS_PAGER", L"" }, { L"zzz", L"x" }, { L"zzz", L"y" },
                                               { L"", L"ignored" }, { L"A=B", L"ignored" } };
    const std::wstring block = BuildEnvironmentBlock(parent, overrides);
    const std::vector<std::wstring> entries = SplitBlock(block);
    const std::vector<std::wstring> expected = { L"=C:=C:\\", L"ABC=2", L"AWS_PAGER=", L"Path=a", L"zzz=y" };
    c.Check(entries == expected, "environment block: case-insensitive later-wins, empty value kept, sorted, bad names skipped");
    c.Check(block.size() >= 2 && block[block.size() - 1] == L'\0' && block[block.size() - 2] == L'\0', "environment block ends with a double NUL");
    c.Check(BuildEnvironmentBlock({}, {}).size() == 2, "empty environment block is two NULs");

    const std::vector<CliEnvVar> fixed = FixedCliEnvironment();
    bool hasEncoding = false, hasFileEncoding = false, hasPager = false;
    for (const CliEnvVar& v : fixed) {
        hasEncoding |= (v.name == L"PYTHONIOENCODING" && v.value == L"utf-8");
        hasFileEncoding |= (v.name == L"AWS_CLI_FILE_ENCODING" && v.value == L"UTF-8");
        hasPager |= (v.name == L"AWS_PAGER" && v.value.empty());
    }
    c.Check(fixed.size() == 3 && hasEncoding && hasFileEncoding && hasPager, "fixed environment: PYTHONIOENCODING / AWS_CLI_FILE_ENCODING / AWS_PAGER=");
}

void TestClassify(Counter& c)
{
    const auto typed = [](const char* code, const char* op, const char* message) {
        return std::string("\nAn error occurred (") + code + ") when calling the " + op + " operation: " + message + "\n";
    };
    AwsError e = ClassifyAwsFailure(0, "");
    c.Check(e.kind == AwsErrorKind::None, "exit 0 is a success");

    e = ClassifyAwsFailure(254, typed("ExpiredTokenException", "DescribeFleetAttributes", "The security token included in the request is expired"));
    c.Check(e.kind == AwsErrorKind::Credentials && e.code == "ExpiredTokenException" && e.operation == "DescribeFleetAttributes" &&
                e.message == "The security token included in the request is expired",
            "ExpiredTokenException -> credentials (code / operation / message)");
    c.Check(ClassifyAwsFailure(254, typed("ExpiredToken", "GetCallerIdentity", "x")).kind == AwsErrorKind::Credentials, "ExpiredToken -> credentials");
    c.Check(ClassifyAwsFailure(254, typed("UnrecognizedClientException", "ListLocations", "The security token included in the request is invalid.")).kind == AwsErrorKind::Credentials,
            "UnrecognizedClientException -> credentials");
    c.Check(ClassifyAwsFailure(254, typed("InvalidClientTokenId", "GetCallerIdentity", "x")).kind == AwsErrorKind::Credentials, "InvalidClientTokenId -> credentials");
    c.Check(ClassifyAwsFailure(254, typed("NotFoundException", "DescribeCompute", "Compute PC-01 not found")).kind == AwsErrorKind::NotFound, "NotFoundException");
    c.Check(ClassifyAwsFailure(254, typed("FleetCapacityExceededException", "CreateGameSession", "no capacity")).kind == AwsErrorKind::CapacityExceeded,
            "FleetCapacityExceededException");
    c.Check(ClassifyAwsFailure(254, typed("ConflictException", "CreateLocation", "exists")).kind == AwsErrorKind::Conflict, "ConflictException");
    c.Check(ClassifyAwsFailure(254, typed("InvalidRequestException", "DeleteFleet", "bad")).kind == AwsErrorKind::InvalidRequest, "InvalidRequestException");
    e = ClassifyAwsFailure(254, typed("AccessDeniedException", "CreateFleet", "not authorized"));
    c.Check(e.kind == AwsErrorKind::Other && e.code == "AccessDeniedException" && e.message == "not authorized", "other codes -> Other (keeps code and message)");

    e = ClassifyAwsFailure(254, "\nAn error occurred (ThrottlingException) when calling the ListLocations operation (reached max retries: 4): Rate exceeded\n");
    c.Check(e.kind == AwsErrorKind::Other && e.operation == "ListLocations" && e.message == "Rate exceeded", "'(reached max retries: 4)' form is parsed");

    c.Check(ClassifyAwsFailure(253, "\nUnable to locate credentials. You can configure credentials by running \"aws configure\".\n").kind == AwsErrorKind::Credentials,
            "'Unable to locate credentials' -> credentials");
    c.Check(ClassifyAwsFailure(255, "Error loading SSO Token: Token for default does not exist\n").kind == AwsErrorKind::Credentials, "SSO token error -> credentials");
    c.Check(ClassifyAwsFailure(255, "Your session has expired. Please reauthenticate using 'aws login'.\n").kind == AwsErrorKind::Credentials, "expired session -> credentials");
    c.Check(ClassifyAwsFailure(253, "\nYou must specify a region. You can also configure your region by running \"aws configure\".\n").kind == AwsErrorKind::NoRegion,
            "missing region");
    e = ClassifyAwsFailure(255, "\r\n\r\nCould not connect to the endpoint URL: \"https://gamelift.x.amazonaws.com/\"\r\nsecond\r\n");
    c.Check(e.kind == AwsErrorKind::Other && e.message == "Could not connect to the endpoint URL: \"https://gamelift.x.amazonaws.com/\"",
            "untyped failure -> Other with the first non-empty line");
    e = ClassifyAwsFailure(7, "");
    c.Check(e.kind == AwsErrorKind::Other && e.message == "aws exited with code 7", "empty stderr -> Other with the exit code");

    c.Check(FirstErrorLines("\n\na\r\nb\nc\nd\n", 3) == "a\nb\nc" && FirstErrorLines("", 3).empty(), "FirstErrorLines takes up to N non-empty lines");
}

void TestTextParsers(Counter& c)
{
    AwsCliVersion v;
    c.Check(ParseAwsCliVersion("aws-cli/2.15.30 Python/3.11.8 Windows/10 exe/AMD64 prompt/off\r\n", v) && v.major == 2 && v.minor == 15 &&
                v.patch == 30 && v.text.rfind("aws-cli/2.15.30", 0) == 0,
            "aws --version (v2)");
    c.Check(ParseAwsCliVersion("aws-cli/1.27.1 Python/3.8.10 Windows/10 botocore/1.29.1", v) && v.major == 1, "aws --version (v1)");
    c.Check(!ParseAwsCliVersion("", v) && !ParseAwsCliVersion("something else", v), "aws --version: garbage is rejected");

    std::string line;
    c.Check(ParseSingleLineText("ap-northeast-1\r\n", line) && line == "ap-northeast-1" && !ParseSingleLineText("\r\n \n", line),
            "configure get region (one line / empty)");

    const std::string uploading = "Zipping C:\\x to C:\\tmp\\y.zip\nUploading C:\\tmp\\y.zip to S3 (21.45%)\rUploading C:\\tmp\\y.zip to S3 (63.00%)\r";
    AwsUploadProgress p = ParseUploadBuildOutput(uploading);
    c.Check(p.percent > 62.99 && p.percent < 63.01 && p.buildId.empty(), "upload-build: progress from CR-separated lines, no Build ID yet");
    p = ParseUploadBuildOutput(uploading + "Uploading C:\\tmp\\y.zip to S3 (100.00%)\nSuccessfully uploaded C:\\x to GameLift.\nBuild ID: build-1111aaaa-2222-3333-4444-555566667777\n");
    c.Check(p.percent > 99.99 && p.buildId == "build-1111aaaa-2222-3333-4444-555566667777", "upload-build: final percent and Build ID");
    p = ParseUploadBuildOutput("");
    c.Check(p.percent < 0.0 && p.buildId.empty(), "upload-build: empty output");
}

void TestJsonParsers(Counter& c)
{
    std::string err;

    AwsCallerIdentity id;
    c.Check(ParseCallerIdentity(R"({"UserId":"AIDAEXAMPLE","Account":"123456789012","Arn":"arn:aws:iam::123456789012:user/dev"})", id, err) &&
                id.account == "123456789012" && id.arn == "arn:aws:iam::123456789012:user/dev" && id.userId == "AIDAEXAMPLE",
            "sts get-caller-identity");
    c.Check(!ParseCallerIdentity("not json", id, err) && !err.empty() && !ParseCallerIdentity("{}", id, err) && !ParseCallerIdentity("[1]", id, err),
            "sts: bad JSON / missing Account / non-object are rejected");

    std::vector<AwsLocation> locations;
    c.Check(ParseListLocations(R"({"Locations":[{"LocationName":"custom-mye-dev","LocationArn":"arn:aws:gamelift:ap-northeast-1:123456789012:location/custom-mye-dev"},{"LocationName":"custom-b"}]})",
                               locations, err) &&
                locations.size() == 2 && locations[0].name == "custom-mye-dev" && locations[0].arn.find("location/") != std::string::npos,
            "list-locations");
    c.Check(ParseListLocations("{}", locations, err) && locations.empty() && ParseListLocations(R"({"Locations":[]})", locations, err) && locations.empty(),
            "list-locations: empty result (key absent / empty array)");
    c.Check(!ParseListLocations(R"({"Locations":"x"})", locations, err), "list-locations: wrong type is rejected");
    AwsLocation location;
    c.Check(ParseCreateLocation(R"({"Location":{"LocationName":"custom-mye-dev","LocationArn":"arn:aws:gamelift:ap-northeast-1:123456789012:location/custom-mye-dev"}})", location, err) &&
                location.name == "custom-mye-dev" && !ParseCreateLocation("{}", location, err),
            "create-location");

    AwsFleetAttributes fleet;
    c.Check(ParseCreateFleet(R"({"FleetAttributes":{"FleetId":"fleet-1111","FleetArn":"arn:aws:gamelift:ap-northeast-1:123456789012:fleet/fleet-1111","Name":"mye-anywhere","Status":"NEW","ComputeType":"ANYWHERE","FleetType":"ON_DEMAND","CreationTime":"2026-11-27T10:00:00.123000+09:00"},"LocationStates":[]})",
                             fleet, err) &&
                fleet.fleetId == "fleet-1111" && fleet.name == "mye-anywhere" && fleet.status == "NEW" && fleet.computeType == "ANYWHERE",
            "create-fleet");
    c.Check(!ParseCreateFleet(R"({"FleetAttributes":{"Name":"x"}})", fleet, err), "create-fleet: missing FleetId is rejected");
    std::vector<AwsFleetAttributes> fleets;
    c.Check(ParseDescribeFleetAttributes(R"({"FleetAttributes":[{"FleetId":"fleet-2222","Name":"mye-ec2","Status":"ACTIVATING","BuildId":"build-9","InstanceType":"c5.large","ComputeType":"EC2"}]})", fleets, err) &&
                fleets.size() == 1 && fleets[0].status == "ACTIVATING" && fleets[0].buildId == "build-9" && fleets[0].instanceType == "c5.large",
            "describe-fleet-attributes");
    c.Check(ParseDescribeFleetAttributes(R"({"FleetAttributes":[]})", fleets, err) && fleets.empty(), "describe-fleet-attributes: gone fleet = empty array");

    std::vector<AwsFleetCapacity> capacities;
    c.Check(ParseDescribeFleetCapacity(R"({"FleetCapacity":[{"FleetId":"fleet-2222","InstanceType":"c5.large","InstanceCounts":{"DESIRED":1,"MINIMUM":0,"MAXIMUM":1,"PENDING":1,"ACTIVE":0,"IDLE":0,"TERMINATING":0},"Location":"ap-northeast-1"}]})",
                                       capacities, err) &&
                capacities.size() == 1 && capacities[0].desired == 1 && capacities[0].maximum == 1 && capacities[0].pending == 1 &&
                capacities[0].Billable() == 1 && capacities[0].location == "ap-northeast-1",
            "describe-fleet-capacity (PENDING counts as billable)");
    c.Check(ParseDescribeFleetCapacity(R"({"FleetCapacity":[{"FleetId":"f","InstanceCounts":{"ACTIVE":2,"PENDING":1,"TERMINATING":3}}]})", capacities, err) &&
                capacities[0].Billable() == 3 && capacities[0].terminating == 3,
            "describe-fleet-capacity: ACTIVE + PENDING, TERMINATING is not counted");
    c.Check(ParseDescribeFleetCapacity(R"({"FleetCapacity":[{"FleetId":"f"}]})", capacities, err) && capacities[0].Billable() == 0, "describe-fleet-capacity: missing counts = 0");

    std::vector<AwsFleetEvent> events;
    c.Check(ParseDescribeFleetEvents(R"({"Events":[{"EventId":"e1","ResourceId":"fleet-2222","EventCode":"SERVER_PROCESS_CRASHED","Message":"Server process crashed","EventTime":"2026-11-27T10:05:00+09:00"},{"EventCode":"FLEET_STATE_ACTIVE","Message":"ok","EventTime":1795000000.5}]})",
                                     events, err) &&
                events.size() == 2 && events[0].eventCode == "SERVER_PROCESS_CRASHED" && events[0].message == "Server process crashed" &&
                events[0].eventTime.rfind("2026-11-27", 0) == 0 && !events[1].eventTime.empty(),
            "describe-fleet-events (string and numeric timestamps)");

    AwsCompute compute;
    const char* const computeJson =
        R"({"Compute":{"FleetId":"fleet-1111","FleetArn":"arn:x","ComputeName":"PC-01","ComputeArn":"arn:aws:gamelift:ap-northeast-1:123456789012:compute/PC-01","IpAddress":"127.0.0.1","ComputeStatus":"Active","Location":"custom-mye-dev","CreationTime":"2026-11-27T10:00:00+09:00","OperatingSystem":"WINDOWS_2016","Type":"ANYWHERE","GameLiftServiceSdkEndpoint":"wss://ap-northeast-1.api.amazongamelift.com"}})";
    c.Check(ParseCompute(computeJson, compute, err) && compute.computeName == "PC-01" && compute.ipAddress == "127.0.0.1" && compute.status == "Active" &&
                compute.location == "custom-mye-dev" && compute.sdkEndpoint == "wss://ap-northeast-1.api.amazongamelift.com" &&
                !ParseCompute(R"({"Compute":{}})", compute, err),
            "register-compute / describe-compute (SDK endpoint)");

    AwsComputeAuthToken token;
    c.Check(ParseComputeAuthToken(R"({"FleetId":"fleet-1111","ComputeName":"PC-01","AuthToken":"abcdef-secret","ExpirationTimeStamp":"2026-11-27T10:15:00+09:00"})", token, err) &&
                token.authToken == "abcdef-secret" && token.computeName == "PC-01" && !ParseComputeAuthToken(R"({"ComputeName":"x"})", token, err),
            "get-compute-auth-token");
    ParseComputeAuthToken(R"({"AuthToken":"abcdef-secret"})", token, err);
    token.Wipe();
    c.Check(token.authToken.empty(), "auth token Wipe clears the value");

    AwsGameSession session;
    c.Check(ParseCreateGameSession(R"({"GameSession":{"GameSessionId":"arn:aws:gamelift:ap-northeast-1::gamesession/fleet-1111/custom-mye-dev/gsess-1","FleetId":"fleet-1111","Status":"ACTIVATING","CurrentPlayerSessionCount":0,"MaximumPlayerSessionCount":4,"Location":"custom-mye-dev","IpAddress":"127.0.0.1","Port":7777}})",
                                   session, err) &&
                session.gameSessionId.find("gsess-1") != std::string::npos && session.status == "ACTIVATING" && session.maxPlayers == 4 &&
                session.port == 7777 && session.ipAddress == "127.0.0.1",
            "create-game-session");
    std::vector<AwsGameSession> sessions;
    c.Check(ParseDescribeGameSessions(R"({"GameSessions":[{"GameSessionId":"g1","Status":"ACTIVE","Port":7777,"IpAddress":"10.0.0.1","DnsName":"ec2-x.compute.amazonaws.com","CurrentPlayerSessionCount":1,"MaximumPlayerSessionCount":4},{"GameSessionId":"g2","Status":"TERMINATED","StatusReason":"FORCE_TERMINATED"}]})",
                                      sessions, err) &&
                sessions.size() == 2 && sessions[0].status == "ACTIVE" && sessions[0].currentPlayers == 1 && sessions[1].statusReason == "FORCE_TERMINATED",
            "describe-game-sessions");
    c.Check(ParseDescribeGameSessions("{}", sessions, err) && sessions.empty(), "describe-game-sessions: none");

    AwsPlayerSession player;
    c.Check(ParseCreatePlayerSession(R"({"PlayerSession":{"PlayerSessionId":"psess-1111","PlayerId":"p-1","GameSessionId":"g1","FleetId":"fleet-1111","Status":"RESERVED","IpAddress":"127.0.0.1","Port":7777}})",
                                     player, err) &&
                player.playerSessionId == "psess-1111" && player.status == "RESERVED" && player.port == 7777 && player.ipAddress == "127.0.0.1" &&
                !ParseCreatePlayerSession(R"({"PlayerSession":{}})", player, err),
            "create-player-session");

    AwsBuild build;
    c.Check(ParseDescribeBuild(R"({"Build":{"BuildId":"build-1111","BuildArn":"arn:x","Name":"mye-server","Version":"1","Status":"READY","SizeOnDisk":12345,"OperatingSystem":"WINDOWS_2022","CreationTime":"2026-11-27T10:00:00+09:00","ServerSdkVersion":"5.6.0"}})", build, err) &&
                build.buildId == "build-1111" && build.status == "READY" && build.operatingSystem == "WINDOWS_2022" && !ParseDescribeBuild(R"({"Build":{}})", build, err),
            "describe-build");
}

// ---------------------------------------------------------------------------------
// 実プロセス (cmd.exe を偽 CLI にする)
// ---------------------------------------------------------------------------------

struct EnvSnapshot
{
    bool present = false;
    std::wstring value;
};

EnvSnapshot ReadEnv(const wchar_t* name)
{
    EnvSnapshot snap;
    wchar_t buffer[2048] = {};
    const DWORD length = GetEnvironmentVariableW(name, buffer, 2048);
    snap.present = length > 0 || GetLastError() != ERROR_ENVVAR_NOT_FOUND;
    if (length > 0 && length < 2048) {
        snap.value.assign(buffer, length);
    }
    return snap;
}

bool SameEnv(const EnvSnapshot& a, const EnvSnapshot& b)
{
    return a.present == b.present && a.value == b.value;
}

// cmd.exe /d /s /c "<command>" を作業ディレクトリ workDir で
CliRunSpec CmdSpec(const std::wstring& command, const fs::path& workDir)
{
    CliRunSpec spec;
    spec.exePath = L"cmd.exe";
    spec.args = { L"/d", L"/s", L"/c", command };
    spec.workDir = workDir.wstring();
    return spec;
}

// Running の間 Poll を回して最終状態を返す (limitMs で打ち切り = まだ Running なら Running を返す)
CliRunState WaitUntilDone(CliProcess& process, uint64_t limitMs)
{
    const uint64_t begin = GetTickCount64();
    while (GetTickCount64() - begin < limitMs) {
        const CliRunState state = process.Poll();
        if (state != CliRunState::Running) {
            return state;
        }
        Sleep(2);
    }
    return process.Poll();
}

std::string NarrowAscii(const std::wstring& wide)
{
    std::string narrow;
    for (const wchar_t ch : wide) {
        narrow.push_back(static_cast<char>(ch));
    }
    return narrow;
}

void TestProcesses(Counter& c)
{
    const fs::path dir = fs::temp_directory_path() / (L"mye_gl_cli_selftest_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    // 1.5 MB 超の入力 (type で標準出力 / 標準エラーへ流す)
    const std::string line(48, 'x');
    size_t bigBytes = 0;
    {
        std::ofstream f(dir / L"big.txt", std::ios::binary);
        for (int i = 0; i < 30000; ++i) {
            f << line << "\r\n";
            bigBytes += line.size() + 2;
        }
    }

    CliProcess process;
    std::string error;

    // stdout / stderr の分離と終了コード
    c.Check(process.Start(CmdSpec(L"echo out-line& >&2 echo err-line& exit /b 3", dir), error), "start: separated streams");
    c.Check(WaitUntilDone(process, 15000) == CliRunState::Finished, "separated streams: finished");
    c.Check(process.ExitCode() == 3, "exit code 3 is reported");
    c.Check(process.StdoutText() == "out-line\r\n", "stdout holds only the stdout line");
    c.Check(process.StderrText() == "err-line\r\n", "stderr holds only the stderr line");

    // 再利用 (前回の出力が残らない) と作業ディレクトリ
    c.Check(process.Start(CmdSpec(L"cd", dir), error) && WaitUntilDone(process, 15000) == CliRunState::Finished && process.ExitCode() == 0 &&
                process.StderrText().empty() && process.StdoutText().find(NarrowAscii(dir.wstring())) != std::string::npos,
            "a finished CliProcess is reusable; the working directory reaches the child");

    // 1 MB 超の標準出力
    c.Check(process.Start(CmdSpec(L"type big.txt", dir), error) && WaitUntilDone(process, 30000) == CliRunState::Finished &&
                process.StdoutText().size() == bigBytes && process.StderrText().empty(),
            "over 1 MB on stdout completes without stalling and is captured whole");

    // 標準エラーが先に大量 → 標準出力も大量 (読み取りを片方ずつ順にやると詰まる並び)
    c.Check(process.Start(CmdSpec(L">&2 type big.txt & type big.txt", dir), error) && WaitUntilDone(process, 30000) == CliRunState::Finished &&
                process.StdoutText().size() == bigBytes && process.StderrText().size() == bigBytes,
            "over 1 MB on both streams: no deadlock, both captured whole");

    // Poll は待たない
    {
        CliRunSpec slow = CmdSpec(L"ping -n 30 127.0.0.1 >nul", dir);
        c.Check(process.Start(slow, error), "start: long-running child");
        const uint64_t begin = GetTickCount64();
        CliRunState state = CliRunState::Idle;
        for (int i = 0; i < 20; ++i) {
            state = process.Poll();
        }
        c.Check(state == CliRunState::Running && GetTickCount64() - begin < 200, "Poll returns immediately while the child runs");

        // 取消: 子 (cmd) と孫 (ping) を止める。孫が stderr を握ったままだと猶予 (2 s) を待つので、その前に終わること
        const uint64_t cancelAt = GetTickCount64();
        process.Cancel();
        const CliRunState after = WaitUntilDone(process, 10000);
        c.Check(after == CliRunState::Cancelled, "Cancel ends the child and reports Cancelled");
        c.Check(GetTickCount64() - cancelAt < 1500, "Cancel stops the grandchild too (the Job Object), without waiting for the drain grace");
    }

    // タイムアウト
    {
        CliRunSpec slow = CmdSpec(L"ping -n 30 127.0.0.1 >nul", dir);
        slow.timeoutMs = 400;
        c.Check(process.Start(slow, error), "start: with a timeout");
        const CliRunState state = WaitUntilDone(process, 10000);
        c.Check(state == CliRunState::TimedOut, "timeout ends the child and reports TimedOut");
        c.Check(process.ElapsedMs() >= 400 && process.ElapsedMs() < 1900, "timeout fires near the limit and takes the grandchild with it");
    }

    // 追加環境の到達・固定環境・親の不変
    {
        const EnvSnapshot beforeValue = ReadEnv(L"MYE_CLI_TEST_VALUE");
        const EnvSnapshot beforeEncoding = ReadEnv(L"PYTHONIOENCODING");
        const EnvSnapshot beforeFileEncoding = ReadEnv(L"AWS_CLI_FILE_ENCODING");
        const EnvSnapshot beforePager = ReadEnv(L"AWS_PAGER");
        const EnvSnapshot systemRoot = ReadEnv(L"SystemRoot");

        CliRunSpec spec = CmdSpec(L"echo %PYTHONIOENCODING% %AWS_CLI_FILE_ENCODING% %MYE_CLI_TEST_VALUE% %SystemRoot%", dir);
        spec.extraEnv = { { L"MYE_CLI_TEST_VALUE", L"abc def" } };
        c.Check(process.Start(spec, error) && WaitUntilDone(process, 15000) == CliRunState::Finished, "start: environment");
        const std::string expected = "utf-8 UTF-8 abc def " + NarrowAscii(systemRoot.value) + "\r\n";
        c.Check(process.StdoutText() == expected, "child sees the fixed variables, the extra variable and the inherited parent variables");
        c.Check(SameEnv(beforeValue, ReadEnv(L"MYE_CLI_TEST_VALUE")) && SameEnv(beforeEncoding, ReadEnv(L"PYTHONIOENCODING")) &&
                    SameEnv(beforeFileEncoding, ReadEnv(L"AWS_CLI_FILE_ENCODING")) && SameEnv(beforePager, ReadEnv(L"AWS_PAGER")),
                "the editor's own environment is unchanged after the call");

        // 呼び出しごとの追加は固定値より後勝ち
        CliRunSpec override = CmdSpec(L"echo %PYTHONIOENCODING%", dir);
        override.extraEnv = { { L"PYTHONIOENCODING", L"ascii" } };
        c.Check(process.Start(override, error) && WaitUntilDone(process, 15000) == CliRunState::Finished && process.StdoutText() == "ascii\r\n",
                "a per-call variable overrides a fixed one");
    }

    // 秘密: 結果を読んだら消せる
    {
        CliRunSpec secret = CmdSpec(L"echo secret-token-value", dir);
        secret.secret = true;
        c.Check(process.Start(secret, error) && WaitUntilDone(process, 15000) == CliRunState::Finished && process.Secret() &&
                    process.StdoutText() == "secret-token-value\r\n",
                "secret call: output is readable after it finishes");
        process.ClearOutput();
        c.Check(process.StdoutText().empty() && process.StderrText().empty(), "ClearOutput empties a secret result");
    }

    // 起動失敗
    {
        CliRunSpec missing;
        missing.exePath = (dir / L"no-such-cli.exe").wstring();
        c.Check(!process.Start(missing, error) && !error.empty() && process.State() == CliRunState::StartFailed, "a missing exe fails to start with a reason");
        CliRunSpec empty;
        c.Check(!process.Start(empty, error) && process.State() == CliRunState::StartFailed, "an empty exe path fails to start");
        c.Check(process.Start(CmdSpec(L"echo again", dir), error) && WaitUntilDone(process, 15000) == CliRunState::Finished,
                "a CliProcess is usable again after a failed start");
    }

    // 実行中の二重 Start は拒否
    {
        c.Check(process.Start(CmdSpec(L"ping -n 30 127.0.0.1 >nul", dir), error), "start: long-running child (double start)");
        c.Check(!process.Start(CmdSpec(L"echo x", dir), error) && process.Running(), "Start while running is refused and the first child keeps running");
        process.Cancel();
        WaitUntilDone(process, 10000);
    }

    // 破棄: 実行中でも子 (と孫) を止めて速やかに戻る
    {
        const uint64_t begin = GetTickCount64();
        {
            CliProcess scoped;
            c.Check(scoped.Start(CmdSpec(L"ping -n 30 127.0.0.1 >nul", dir), error), "start: child for the destructor test");
        }
        c.Check(GetTickCount64() - begin < 4000, "destroying a running CliProcess kills the child and returns promptly");
    }

    fs::remove_all(dir, ec);
}

} // namespace

bool RunGameLiftCliSelfTest()
{
    Counter c;
    TestQuotingRoundTrip(c);
    TestValidators(c);
    TestCommands(c);
    TestEnvironmentBlock(c);
    TestClassify(c);
    TestTextParsers(c);
    TestJsonParsers(c);
    if (c.failCount != 0) {
        MYE_LOG_ERROR("gameliftcli: %d check(s) failed", c.failCount);
    }
    return c.failCount == 0;
}

bool RunGameLiftCliProcessSelfTest()
{
    Counter c;
    TestProcesses(c);
    if (c.failCount != 0) {
        MYE_LOG_ERROR("gameliftcliprocess: %d check(s) failed", c.failCount);
    }
    return c.failCount == 0;
}

} // namespace mye
