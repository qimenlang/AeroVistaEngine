#include "function/config/EngineConfig.h"

#include <aerovista/sync/SyncConfig.h>
#include <aerovista/sync/SyncJson.h>

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

using aerovista::sync::sync_json::JsonArray;
using aerovista::sync::sync_json::JsonObject;
using aerovista::sync::sync_json::JsonValue;

// 通用 JSON 辅助（find/require*/rejectUnknownKeys）与 hostConfig/igConfig 解析
// 均来自 sync 库（SyncJson.h / SyncConfig.h），引擎侧不再重复实现。
using aerovista::sync::sync_json::find;
using aerovista::sync::sync_json::parseJsonText;
using aerovista::sync::sync_json::rejectNull;
using aerovista::sync::sync_json::rejectUnknownKeys;
using aerovista::sync::sync_json::requireBool;
using aerovista::sync::sync_json::requireInt;
using aerovista::sync::sync_json::requireNumber;
using aerovista::sync::sync_json::requireObject;
using aerovista::sync::sync_json::requireString;

namespace
{
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
        if (find(obj, key) == nullptr)
            return fallback;
        return requireInt(obj, key);
    }

    std::string parseOptionalString(const JsonObject& obj, const char* key, std::string fallback)
    {
        if (find(obj, key) == nullptr)
            return fallback;
        return requireString(obj, key);
    }

    void validateIgEndpointPairing(const EngineChannelConfig& cfg, bool hasRequireConnectedIg)
    {
        if (hasRequireConnectedIg && !cfg.igConfig)
            throw std::runtime_error("requireConnectedIg without igConfig is invalid");
        // 同步只 LLA：本地笛卡尔场景参与同步的 fail-fast 无法在配置加载期判定
        // （「场景有无 EllipsoidModel」要 loadScene 后才知道），移至 Engine::ensureEllipsoidModel。
    }

    std::string basenameOfModel(const std::string& modelPath)
    {
        const auto slash = modelPath.find_last_of("/\\");
        return slash == std::string::npos ? modelPath : modelPath.substr(slash + 1);
    }

    Vec3Config requireVec3Array(const JsonObject& obj, const char* key)
    {
        const JsonValue* v = find(obj, key);
        if (!v)
            throw std::runtime_error(std::string("missing/invalid array: ") + key);
        rejectNull(*v, key);
        if (!v->is_array())
            throw std::runtime_error(std::string("missing/invalid array: ") + key);
        const JsonArray& arr = *v;
        if (arr.size() != 3)
            throw std::runtime_error(std::string("array length must be 3: ") + key);
        Vec3Config out;
        for (std::size_t i = 0; i < 3; ++i)
        {
            if (!arr[i].is_number())
                throw std::runtime_error(std::string("array elements must be numbers: ") + key);
        }
        out.x = arr[0].get<double>();
        out.y = arr[1].get<double>();
        out.z = arr[2].get<double>();
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
        const JsonObject& llaObj = requireObject(*llaValue, "lla");
        rejectUnknownKeys(llaObj, {"lat", "lon", "alt"});
        EllipsoidPoseConfig pose;
        pose.lla.x = requireNumber(llaObj, "lat");
        pose.lla.y = requireNumber(llaObj, "lon");
        pose.lla.z = requireNumber(llaObj, "alt");
        pose.eulerYprDeg = requireVec3Array(obj, "eulerYprDeg");
        return pose;
    }

    void parseDualPose(const JsonObject& poseObj, bool& hasLocal, LocalPoseConfig& local,
                       bool& hasEllipsoid, EllipsoidPoseConfig& ellipsoid)
    {
        rejectUnknownKeys(poseObj, {"local", "ellipsoid"});
        hasLocal = false;
        hasEllipsoid = false;
        if (const JsonValue* v = find(poseObj, "local"))
        {
            hasLocal = true;
            local = parseLocalPose(requireObject(*v, "local"));
        }
        if (const JsonValue* v = find(poseObj, "ellipsoid"))
        {
            hasEllipsoid = true;
            ellipsoid = parseEllipsoidPose(requireObject(*v, "ellipsoid"));
        }
        // 双轨自由解析；运行时按「场景有无 EllipsoidModel」选半，不在加载期强制选半。
    }

    EntityInitialState parseEntityInitialState(const JsonObject& obj)
    {
        const JsonValue* v = find(obj, "initialEntityState");
        if (!v)
            return EntityInitialState::ACTIVE;
        rejectNull(*v, "initialEntityState");
        if (!v->is_string())
            throw std::runtime_error("missing/invalid string: initialEntityState");
        const std::string s = v->get<std::string>();
        if (s == "Active")
            return EntityInitialState::ACTIVE;
        if (s == "Standby")
            return EntityInitialState::STANDBY;
        throw std::runtime_error("invalid initialEntityState (only \"Active\" or \"Standby\"): " + s);
    }

    EntityConfig parseEntityItem(const JsonObject& obj)
    {
        rejectUnknownKeys(obj, {"id", "name", "model", "initialEntityState", "pose"});
        EntityConfig entity;
        entity.id = requireInt(obj, "id");
        if (entity.id < 1 || entity.id > 65535)
            throw std::runtime_error("entity id out of range 1..65535");
        entity.model = requireString(obj, "model");
        if (entity.model.empty())
            throw std::runtime_error("entities[].model must be non-empty");
        entity.name = parseOptionalString(obj, "name", basenameOfModel(entity.model));
        entity.initialEntityState = parseEntityInitialState(obj);
        if (const JsonValue* poseValue = find(obj, "pose"))
        {
            entity.hasPose = true;
            parseDualPose(requireObject(*poseValue, "pose"), entity.hasPoseLocal, entity.localPose,
                          entity.hasPoseEllipsoid, entity.ellipsoidPose);
        }
        return entity;
    }

    std::vector<EntityConfig> parseEntitiesArray(const JsonValue& value)
    {
        rejectNull(value, "entities");
        if (!value.is_array())
            throw std::runtime_error("entities must be an array");
        const JsonArray& arr = value;
        if (arr.empty())
            throw std::runtime_error("entities must not be empty");

        std::vector<EntityConfig> entities;
        entities.reserve(arr.size());
        std::unordered_set<int> seenIds;
        for (const JsonValue& item : arr)
        {
            const EntityConfig entity = parseEntityItem(requireObject(item, "entities[]"));
            if (!seenIds.insert(entity.id).second)
                throw std::runtime_error("duplicate entity id");
            entities.push_back(entity);
        }
        return entities;
    }

    CameraConfig parseCamera(const JsonObject& obj)
    {
        rejectUnknownKeys(obj, {"pose"});
        CameraConfig camera;
        if (const JsonValue* poseValue = find(obj, "pose"))
        {
            camera.hasPose = true;
            parseDualPose(requireObject(*poseValue, "pose"), camera.hasPoseLocal, camera.localPose,
                          camera.hasPoseEllipsoid, camera.ellipsoidPose);
        }
        return camera;
    }

    void parseModelEntityMutex(const JsonObject& root, EngineChannelConfig& cfg)
    {
        const bool hasModelKey = find(root, "model") != nullptr;
        const bool hasEntityKey = find(root, "entity") != nullptr;
        if (hasEntityKey)
            throw std::runtime_error("singular entity is not supported; use a separate entities file (entitiesFilePath)");
        if (hasModelKey)
            cfg.model = requireString(root, "model");
        if (const JsonValue* v = find(root, "entitiesFilePath"))
        {
            rejectNull(*v, "entitiesFilePath");
            if (!v->is_string())
                throw std::runtime_error("missing/invalid string: entitiesFilePath");
            cfg.entitiesFilePath = v->get<std::string>();
        }
    }

    SyncSystemConfig parseSyncSystemConfig(const JsonObject& obj)
    {
        rejectUnknownKeys(obj, {"channelId", "offsetDeg", "requireConnectedIg"});
        SyncSystemConfig ss;
        ss.channelId = parseOptionalInt(obj, "channelId", ss.channelId);
        if (const JsonValue* v = find(obj, "offsetDeg"))
            ss.offsetDeg = parseOffsetDeg(requireObject(*v, "offsetDeg"));
        if (find(obj, "requireConnectedIg") != nullptr)
            ss.requireConnectedIg = requireBool(obj, "requireConnectedIg");
        return ss;
    }

    EngineChannelConfig parseConfig(const JsonObject& root)
    {
        // engine 配置含 hostConfig 属未知键拒绝（Host 用 loadHostConfig）。
        rejectUnknownKeys(root, {"syncSystem", "igConfig", "model", "window",
                                 "injectEllipsoidIfMissing", "entitiesFilePath", "camera"});

        EngineChannelConfig cfg;

        if (const JsonValue* v = find(root, "syncSystem"))
            cfg.syncSystem = parseSyncSystemConfig(requireObject(*v, "syncSystem"));

        if (const JsonValue* v = find(root, "igConfig"))
        {
            cfg.igConfig = parseIgConfig(requireObject(*v, "igConfig"));
        }

        if (const JsonValue* v = find(root, "window"))
            cfg.window = parseWindow(requireObject(*v, "window"));

        if (const JsonValue* v = find(root, "injectEllipsoidIfMissing"))
        {
            rejectNull(*v, "injectEllipsoidIfMissing");
            if (!v->is_boolean())
                throw std::runtime_error("missing/invalid bool: injectEllipsoidIfMissing");
            cfg.injectEllipsoidIfMissing = v->get<bool>();
        }

        parseModelEntityMutex(root, cfg);

        if (const JsonValue* v = find(root, "camera"))
        {
            cfg.camera = parseCamera(requireObject(*v, "camera"));
        }

        validateIgEndpointPairing(cfg, cfg.syncSystem.requireConnectedIg);
        return cfg;
    }
} // namespace

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
        const JsonValue rootValue = parseJsonText(oss.str());
        if (!rootValue.is_object())
            throw std::runtime_error("root must be a JSON object");

        out = parseConfig(rootValue);
        return true;
    }
    catch (const std::exception& ex)
    {
        if (error)
            *error = ex.what();
        return false;
    }
}

bool loadEntitiesFile(const std::string& path, std::vector<EntityConfig>& out, std::string* error)
{
    try
    {
        std::ifstream in(path);
        if (!in)
        {
            if (error)
                *error = "failed to open entities file: " + path;
            return false;
        }

        std::ostringstream oss;
        oss << in.rdbuf();
        const JsonValue rootValue = parseJsonText(oss.str());
        if (!rootValue.is_object())
            throw std::runtime_error("entities file root must be a JSON object");

        const JsonObject& root = rootValue;
        rejectUnknownKeys(root, {"entities"});
        const JsonValue* entitiesValue = find(root, "entities");
        if (!entitiesValue)
            throw std::runtime_error("missing key: entities");
        out = parseEntitiesArray(*entitiesValue);
        return true;
    }
    catch (const std::exception& ex)
    {
        if (error)
            *error = ex.what();
        return false;
    }
}
