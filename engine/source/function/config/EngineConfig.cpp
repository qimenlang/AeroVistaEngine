#include "function/config/EngineConfig.h"

#include <aerovista/sync/SyncConfig.h>
#include <aerovista/sync/SyncJson.h>

#include <cmath>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

using aerovista::sync::sync_json::JsonArray;
using aerovista::sync::sync_json::JsonObject;
using aerovista::sync::sync_json::JsonParser;
using aerovista::sync::sync_json::JsonValue;

// 通用 JSON 辅助（find/require*/rejectUnknownKeys）与 hostConfig/igConfig 解析
// 均来自 sync 库（SyncJson.h / SyncConfig.h），引擎侧不再重复实现。
using aerovista::sync::sync_json::find;
using aerovista::sync::sync_json::rejectNull;
using aerovista::sync::sync_json::rejectUnknownKeys;
using aerovista::sync::sync_json::requireBool;
using aerovista::sync::sync_json::requireInt;
using aerovista::sync::sync_json::requireNumber;
using aerovista::sync::sync_json::requireObjectValue;
using aerovista::sync::sync_json::requireString;
using aerovista::sync::sync_json::requireValue;

namespace
{
    HostEyeStalePolicy parseStalePolicy(const std::string& text)
    {
        if (text == "ReuseLast")
            return HostEyeStalePolicy::REUSE_LAST;
        if (text == "Freeze")
            return HostEyeStalePolicy::FREEZE;
        throw std::runtime_error("invalid hostEyeStalePolicy: " + text);
    }

    OffsetDeg parseOffsetDeg(const JsonObject& obj)
    {
        rejectUnknownKeys(obj, {"yaw", "pitch", "roll"});
        OffsetDeg offset;
        offset.yaw = requireNumber(obj, "yaw");
        offset.pitch = requireNumber(obj, "pitch");
        offset.roll = requireNumber(obj, "roll");
        return offset;
    }

    WindowConfig parseWindow(const JsonObject& obj)
    {
        rejectUnknownKeys(obj, {"x", "y", "width", "height"});
        WindowConfig window;
        window.x = requireInt(obj, "x");
        window.y = requireInt(obj, "y");
        window.width = requireInt(obj, "width");
        window.height = requireInt(obj, "height");
        return window;
    }

    int parseOptionalInt(const JsonObject& obj, const char* key, int fallback)
    {
        const JsonValue* v = find(obj, key);
        if (!v)
            return fallback;
        rejectNull(*v, key);
        if (!v->isNumber())
            throw std::runtime_error(std::string("missing/invalid number: ") + key);
        return static_cast<int>(v->asNumber());
    }

    std::string parseOptionalString(const JsonObject& obj, const char* key, std::string fallback)
    {
        const JsonValue* v = find(obj, key);
        if (!v)
            return fallback;
        rejectNull(*v, key);
        if (!v->isString())
            throw std::runtime_error(std::string("missing/invalid string: ") + key);
        return v->asString();
    }

    void validateIgEndpointPairing(const EngineChannelConfig& cfg, bool hasRequireIgConnect)
    {
        if (hasRequireIgConnect && !cfg.hasIgConfig)
            throw std::runtime_error("requireIgConnect without igConfig is invalid");
    }

    std::string basenameOfModel(const std::string& modelPath)
    {
        const auto slash = modelPath.find_last_of("/\\");
        return slash == std::string::npos ? modelPath : modelPath.substr(slash + 1);
    }

    int requireStrictInt(const JsonObject& obj, const char* key)
    {
        const JsonValue* v = find(obj, key);
        if (!v)
            throw std::runtime_error(std::string("missing/invalid int: ") + key);
        rejectNull(*v, key);
        if (!v->isNumber())
            throw std::runtime_error(std::string("missing/invalid int: ") + key);
        const double n = v->asNumber();
        if (n != std::floor(n))
            throw std::runtime_error(std::string("missing/invalid int: ") + key);
        return static_cast<int>(n);
    }

    Vec3Config requireVec3Array(const JsonObject& obj, const char* key)
    {
        const JsonValue* v = find(obj, key);
        if (!v)
            throw std::runtime_error(std::string("missing/invalid array: ") + key);
        rejectNull(*v, key);
        if (!v->isArray())
            throw std::runtime_error(std::string("missing/invalid array: ") + key);
        const JsonArray& arr = v->asArray();
        if (arr.size() != 3)
            throw std::runtime_error(std::string("array length must be 3: ") + key);
        Vec3Config out;
        for (std::size_t i = 0; i < 3; ++i)
        {
            if (!arr[i].isNumber())
                throw std::runtime_error(std::string("array elements must be numbers: ") + key);
        }
        out.x = arr[0].asNumber();
        out.y = arr[1].asNumber();
        out.z = arr[2].asNumber();
        return out;
    }

    LocalPoseConfig parseLocalPose(const JsonObject& obj)
    {
        rejectUnknownKeys(obj, {"position", "eulerYprDeg"});
        LocalPoseConfig pose;
        pose.position = requireVec3Array(obj, "position");
        pose.eulerYprDeg = requireVec3Array(obj, "eulerYprDeg");
        return pose;
    }

    EllipsoidPoseConfig parseEllipsoidPose(const JsonObject& obj)
    {
        rejectUnknownKeys(obj, {"lla", "eulerYprDeg"});
        const JsonValue* llaValue = find(obj, "lla");
        if (!llaValue)
            throw std::runtime_error("missing/invalid object: lla");
        const JsonObject& llaObj = requireObjectValue(*llaValue, "lla");
        rejectUnknownKeys(llaObj, {"lat", "lon", "alt"});
        EllipsoidPoseConfig pose;
        pose.lla.x = requireNumber(llaObj, "lat");
        pose.lla.y = requireNumber(llaObj, "lon");
        pose.lla.z = requireNumber(llaObj, "alt");
        pose.eulerYprDeg = requireVec3Array(obj, "eulerYprDeg");
        return pose;
    }

    void parseDualPose(const JsonObject& poseObj, CoordFrameIntent frame, bool& hasLocal, LocalPoseConfig& local,
                       bool& hasEllipsoid, EllipsoidPoseConfig& ellipsoid)
    {
        rejectUnknownKeys(poseObj, {"local", "ellipsoid"});
        hasLocal = false;
        hasEllipsoid = false;
        if (const JsonValue* v = find(poseObj, "local"))
        {
            hasLocal = true;
            local = parseLocalPose(requireObjectValue(*v, "local"));
        }
        if (const JsonValue* v = find(poseObj, "ellipsoid"))
        {
            hasEllipsoid = true;
            ellipsoid = parseEllipsoidPose(requireObjectValue(*v, "ellipsoid"));
        }
        if (frame == CoordFrameIntent::LOCAL && !hasLocal)
            throw std::runtime_error("pose.local required when coordFrame is Local");
        if (frame == CoordFrameIntent::ELLIPSOID && !hasEllipsoid)
            throw std::runtime_error("pose.ellipsoid required when coordFrame is Ellipsoid");
    }

    EntityConfig parseEntityItem(const JsonObject& obj, CoordFrameIntent frame)
    {
        rejectUnknownKeys(obj, {"id", "name", "model", "pose"});
        EntityConfig entity;
        entity.id = requireStrictInt(obj, "id");
        entity.model = requireString(obj, "model");
        if (entity.model.empty())
            throw std::runtime_error("entities[].model must be non-empty");
        entity.name = parseOptionalString(obj, "name", basenameOfModel(entity.model));
        if (const JsonValue* poseValue = find(obj, "pose"))
        {
            entity.hasPose = true;
            parseDualPose(requireObjectValue(*poseValue, "pose"), frame, entity.hasPoseLocal, entity.localPose,
                          entity.hasPoseEllipsoid, entity.ellipsoidPose);
        }
        return entity;
    }

    std::vector<EntityConfig> parseEntitiesArray(const JsonValue& value, CoordFrameIntent frame)
    {
        rejectNull(value, "entities");
        if (!value.isArray())
            throw std::runtime_error("entities must be an array");
        const JsonArray& arr = value.asArray();
        if (arr.empty())
            throw std::runtime_error("entities must not be empty");

        std::vector<EntityConfig> entities;
        entities.reserve(arr.size());
        std::unordered_set<int> seenIds;
        for (const JsonValue& item : arr)
        {
            const EntityConfig entity = parseEntityItem(requireObjectValue(item, "entities[]"), frame);
            if (!seenIds.insert(entity.id).second)
                throw std::runtime_error("duplicate entity id");
            entities.push_back(entity);
        }
        return entities;
    }

    CameraConfig parseCamera(const JsonObject& obj, CoordFrameIntent frame)
    {
        rejectUnknownKeys(obj, {"pose"});
        CameraConfig camera;
        if (const JsonValue* poseValue = find(obj, "pose"))
        {
            camera.hasPose = true;
            parseDualPose(requireObjectValue(*poseValue, "pose"), frame, camera.hasPoseLocal, camera.localPose,
                          camera.hasPoseEllipsoid, camera.ellipsoidPose);
        }
        return camera;
    }

    void parseModelEntityMutex(const JsonObject& root, EngineChannelConfig& cfg)
    {
        const bool hasModelKey = find(root, "model") != nullptr;
        const bool hasEntityKey = find(root, "entity") != nullptr;
        const bool hasEntitiesKey = find(root, "entities") != nullptr;
        const int presentCount = static_cast<int>(hasModelKey) + static_cast<int>(hasEntityKey) +
                                 static_cast<int>(hasEntitiesKey);
        if (presentCount > 1)
            throw std::runtime_error("model, entity, and entities are mutually exclusive");
        if (hasEntityKey)
            throw std::runtime_error("singular entity is not supported; use entities");
        if (hasEntitiesKey)
            cfg.entities = parseEntitiesArray(*find(root, "entities"), cfg.coordFrame);
        if (hasModelKey)
            cfg.model = requireString(root, "model");
    }

    SyncSystemConfig parseSyncSystemConfig(const JsonObject& obj)
    {
        rejectUnknownKeys(obj, {"channelId", "offsetDeg", "hostEyeStalePolicy", "requireIgConnect"});
        SyncSystemConfig ss;
        ss.channelId = parseOptionalInt(obj, "channelId", ss.channelId);
        if (const JsonValue* v = find(obj, "offsetDeg"))
            ss.offsetDeg = parseOffsetDeg(requireObjectValue(*v, "offsetDeg"));
        if (find(obj, "hostEyeStalePolicy") != nullptr)
            ss.hostEyeStalePolicy = parseStalePolicy(parseOptionalString(obj, "hostEyeStalePolicy", ""));
        if (find(obj, "requireIgConnect") != nullptr)
            ss.requireIgConnect = requireBool(obj, "requireIgConnect");
        return ss;
    }

    EngineChannelConfig parseConfig(const JsonObject& root)
    {
        rejectUnknownKeys(root, {"syncSystem", "channelId", "offsetDeg", "igConfig", "hostConfig", "model", "window",
                                 "hostEyeStalePolicy", "requireIgConnect", "coordFrame", "entities", "entity",
                                 "camera"});

        EngineChannelConfig cfg;

        // syncSystem 组（新）：存在则用之，并同步到旧扁平字段（旧访问点不变）。
        if (const JsonValue* v = find(root, "syncSystem"))
        {
            cfg.syncSystem = parseSyncSystemConfig(requireObjectValue(*v, "syncSystem"));
            cfg.channelId = cfg.syncSystem.channelId;
            cfg.offsetDeg = cfg.syncSystem.offsetDeg;
            cfg.hostEyeStalePolicy = cfg.syncSystem.hostEyeStalePolicy;
            cfg.requireIgConnect = cfg.syncSystem.requireIgConnect;
        }
        // 旧扁平字段（兼容回退）：仅当无 syncSystem 组时按旧逻辑解析。
        if (find(root, "syncSystem") == nullptr)
        {
            cfg.channelId = parseOptionalInt(root, "channelId", cfg.channelId);
            if (const JsonValue* v = find(root, "offsetDeg"))
                cfg.offsetDeg = parseOffsetDeg(requireObjectValue(*v, "offsetDeg"));
            if (find(root, "hostEyeStalePolicy") != nullptr)
                cfg.hostEyeStalePolicy = parseStalePolicy(parseOptionalString(root, "hostEyeStalePolicy", ""));
            if (find(root, "requireIgConnect") != nullptr)
                cfg.requireIgConnect = requireBool(root, "requireIgConnect");
        }

        if (const JsonValue* v = find(root, "hostConfig"))
        {
            cfg.hasHostConfig = true;
            cfg.hostConfig = parseHostConfig(requireObjectValue(*v, "hostConfig"));
        }

        if (const JsonValue* v = find(root, "igConfig"))
        {
            cfg.hasIgConfig = true;
            cfg.igConfig = parseIgConfig(requireObjectValue(*v, "igConfig"));
        }

        if (const JsonValue* v = find(root, "window"))
            cfg.window = parseWindow(requireObjectValue(*v, "window"));

        if (const JsonValue* v = find(root, "coordFrame"))
        {
            rejectNull(*v, "coordFrame");
            if (!v->isString())
                throw std::runtime_error("missing/invalid string: coordFrame");
            const std::string s = v->asString();
            if (s == "Local")
                cfg.coordFrame = CoordFrameIntent::LOCAL;
            else if (s == "Ellipsoid")
                cfg.coordFrame = CoordFrameIntent::ELLIPSOID;
            else
                throw std::runtime_error("coordFrame must be \"Local\" or \"Ellipsoid\"");
        }

        parseModelEntityMutex(root, cfg);

        if (const JsonValue* v = find(root, "camera"))
        {
            cfg.hasCamera = true;
            cfg.camera = parseCamera(requireObjectValue(*v, "camera"), cfg.coordFrame);
        }

        validateIgEndpointPairing(cfg, cfg.requireIgConnect);
        return cfg;
    }
} // namespace

SyncRoleConfig EngineChannelConfig::toSyncRole() const
{
    SyncRoleConfig role;
    role.enableHost = enableHost();
    role.enableIg = enableIg();
    role.hostConfig = hostConfig;
    role.igConfig = igConfig;
    return role;
}

bool loadEngineChannelConfig(const std::string& path, EngineChannelConfig& out, std::string* error)
{
    try
    {
        std::ifstream in(path);
        if (!in)
        {
            if (error)
                *error = "failed to open config: " + path;
            return false;
        }

        std::ostringstream oss;
        oss << in.rdbuf();
        std::string text = oss.str();
        // windows上读取json文件时，去掉 UTF-8 文件开头的 BOM，避免解析失败
        if (text.size() >= 3 &&
            static_cast<unsigned char>(text[0]) == 0xEF &&
            static_cast<unsigned char>(text[1]) == 0xBB &&
            static_cast<unsigned char>(text[2]) == 0xBF)
        {
            text.erase(0, 3);
        }

        JsonParser parser(std::move(text));
        const JsonValue rootValue = parser.parse();
        if (!rootValue.isObject())
            throw std::runtime_error("root must be a JSON object");

        out = parseConfig(rootValue.asObject());
        return true;
    }
    catch (const std::exception& ex)
    {
        if (error)
            *error = ex.what();
        return false;
    }
}
