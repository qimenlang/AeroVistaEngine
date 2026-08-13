#include "function/config/EngineConfig.h"

#include <cctype>
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

namespace
{
    struct JsonNull
    {
    };

    struct JsonValue;
    using JsonObject = std::unordered_map<std::string, JsonValue>;
    using JsonArray = std::vector<JsonValue>;

    struct JsonValue
    {
        using Storage = std::variant<JsonNull, bool, double, std::string, JsonArray, JsonObject>;
        Storage data;

        bool isNull() const { return std::holds_alternative<JsonNull>(data); }
        bool isObject() const { return std::holds_alternative<JsonObject>(data); }
        bool isArray() const { return std::holds_alternative<JsonArray>(data); }
        bool isString() const { return std::holds_alternative<std::string>(data); }
        bool isNumber() const { return std::holds_alternative<double>(data); }
        bool isBool() const { return std::holds_alternative<bool>(data); }

        const JsonObject& asObject() const { return std::get<JsonObject>(data); }
        const JsonArray& asArray() const { return std::get<JsonArray>(data); }
        const std::string& asString() const { return std::get<std::string>(data); }
        double asNumber() const { return std::get<double>(data); }
        bool asBool() const { return std::get<bool>(data); }
    };

    class JsonParser
    {
    public:
        explicit JsonParser(std::string text) :
            _text(std::move(text)) {}

        JsonValue parse()
        {
            skipWs();
            auto value = parseValue();
            skipWs();
            if (_pos != _text.size())
                throw std::runtime_error("trailing data after JSON value");
            return value;
        }

    private:
        std::string _text;
        std::size_t _pos = 0;

        void skipWs()
        {
            while (_pos < _text.size() && std::isspace(static_cast<unsigned char>(_text[_pos])))
                ++_pos;
        }

        char peek() const
        {
            if (_pos >= _text.size())
                throw std::runtime_error("unexpected end of JSON");
            return _text[_pos];
        }

        char get()
        {
            const char c = peek();
            ++_pos;
            return c;
        }

        bool match(char expected)
        {
            skipWs();
            if (_pos < _text.size() && _text[_pos] == expected)
            {
                ++_pos;
                return true;
            }
            return false;
        }

        void expect(char expected)
        {
            skipWs();
            if (!match(expected))
                throw std::runtime_error(std::string("expected '") + expected + "'");
        }

        JsonValue parseValue()
        {
            skipWs();
            const char c = peek();
            if (c == '{')
                return JsonValue{parseObject()};
            if (c == '[')
                return JsonValue{parseArray()};
            if (c == '"')
                return JsonValue{parseString()};
            if (c == 't' || c == 'f')
                return JsonValue{parseBool()};
            if (c == 'n')
            {
                parseLiteral("null");
                return JsonValue{JsonNull{}};
            }
            if (c == '-' || std::isdigit(static_cast<unsigned char>(c)))
                return JsonValue{parseNumber()};
            throw std::runtime_error("invalid JSON value");
        }

        JsonObject parseObject()
        {
            expect('{');
            JsonObject obj;
            skipWs();
            if (match('}'))
                return obj;

            while (true)
            {
                skipWs();
                const std::string key = parseString();
                expect(':');
                obj.emplace(key, parseValue());
                skipWs();
                if (match('}'))
                    break;
                expect(',');
            }
            return obj;
        }

        JsonArray parseArray()
        {
            expect('[');
            JsonArray arr;
            skipWs();
            if (match(']'))
                return arr;

            while (true)
            {
                arr.push_back(parseValue());
                skipWs();
                if (match(']'))
                    break;
                expect(',');
            }
            return arr;
        }

        std::string parseString()
        {
            expect('"');
            std::string out;
            while (true)
            {
                if (_pos >= _text.size())
                    throw std::runtime_error("unterminated string");
                const char c = get();
                if (c == '"')
                    break;
                if (c == '\\')
                {
                    if (_pos >= _text.size())
                        throw std::runtime_error("unterminated escape");
                    const char e = get();
                    switch (e)
                    {
                    case '"':
                    case '\\':
                    case '/':
                        out.push_back(e);
                        break;
                    case 'b':
                        out.push_back('\b');
                        break;
                    case 'f':
                        out.push_back('\f');
                        break;
                    case 'n':
                        out.push_back('\n');
                        break;
                    case 'r':
                        out.push_back('\r');
                        break;
                    case 't':
                        out.push_back('\t');
                        break;
                    default:
                        throw std::runtime_error("unsupported escape");
                    }
                }
                else
                {
                    out.push_back(c);
                }
            }
            return out;
        }

        bool curIs(char c) const
        {
            return _pos < _text.size() && _text[_pos] == c;
        }

        bool curIsDigit() const
        {
            return _pos < _text.size() && std::isdigit(static_cast<unsigned char>(_text[_pos]));
        }

        void skipDigits()
        {
            while (curIsDigit())
                ++_pos;
        }

        void parseFractionPart()
        {
            if (!curIs('.'))
                return;
            ++_pos;
            skipDigits();
        }

        void parseExponentPart()
        {
            if (!curIs('e') && !curIs('E'))
                return;
            ++_pos;
            if (curIs('+'))
                ++_pos;
            else if (curIs('-'))
                ++_pos;
            skipDigits();
        }

        double parseNumber()
        {
            skipWs();
            const std::size_t begin = _pos;
            if (curIs('-'))
                ++_pos;
            skipDigits();
            parseFractionPart();
            parseExponentPart();
            if (begin == _pos)
                throw std::runtime_error("invalid number");
            return std::stod(_text.substr(begin, _pos - begin));
        }

        bool parseBool()
        {
            if (_text.compare(_pos, 4, "true") == 0)
            {
                _pos += 4;
                return true;
            }
            if (_text.compare(_pos, 5, "false") == 0)
            {
                _pos += 5;
                return false;
            }
            throw std::runtime_error("invalid boolean");
        }

        void parseLiteral(const char* lit)
        {
            const std::size_t n = std::char_traits<char>::length(lit);
            if (_text.compare(_pos, n, lit) != 0)
                throw std::runtime_error(std::string("expected ") + lit);
            _pos += n;
        }
    };

    const JsonValue* find(const JsonObject& obj, const char* key)
    {
        const auto it = obj.find(key);
        return it == obj.end() ? nullptr : &it->second;
    }

    void rejectUnknownKeys(const JsonObject& obj, std::initializer_list<const char*> allowed)
    {
        for (const auto& entry : obj)
        {
            bool ok = false;
            for (const char* key : allowed)
            {
                if (entry.first == key)
                {
                    ok = true;
                    break;
                }
            }
            if (!ok)
                throw std::runtime_error("unknown key: " + entry.first);
        }
    }

    void rejectNull(const JsonValue& v, const char* key)
    {
        if (v.isNull())
            throw std::runtime_error(std::string("null not allowed: ") + key);
    }

    double requireNumber(const JsonObject& obj, const char* key)
    {
        const JsonValue* v = find(obj, key);
        if (!v)
            throw std::runtime_error(std::string("missing/invalid number: ") + key);
        rejectNull(*v, key);
        if (!v->isNumber())
            throw std::runtime_error(std::string("missing/invalid number: ") + key);
        return v->asNumber();
    }

    int requireInt(const JsonObject& obj, const char* key)
    {
        return static_cast<int>(requireNumber(obj, key));
    }

    std::string requireString(const JsonObject& obj, const char* key)
    {
        const JsonValue* v = find(obj, key);
        if (!v)
            throw std::runtime_error(std::string("missing/invalid string: ") + key);
        rejectNull(*v, key);
        if (!v->isString())
            throw std::runtime_error(std::string("missing/invalid string: ") + key);
        return v->asString();
    }

    bool requireBool(const JsonObject& obj, const char* key)
    {
        const JsonValue* v = find(obj, key);
        if (!v)
            throw std::runtime_error(std::string("missing/invalid bool: ") + key);
        rejectNull(*v, key);
        if (!v->isBool())
            throw std::runtime_error(std::string("missing/invalid bool: ") + key);
        return v->asBool();
    }

    const JsonObject& requireObjectValue(const JsonValue& v, const char* key)
    {
        rejectNull(v, key);
        if (!v.isObject())
            throw std::runtime_error(std::string("missing/invalid object: ") + key);
        return v.asObject();
    }

    HostEyeStalePolicy parseStalePolicy(const std::string& text)
    {
        if (text == "ReuseLast")
            return HostEyeStalePolicy::REUSE_LAST;
        if (text == "Freeze")
            return HostEyeStalePolicy::FREEZE;
        throw std::runtime_error("invalid hostEyeStalePolicy: " + text);
    }

    HostConfig parseHostConfig(const JsonObject& obj)
    {
        rejectUnknownKeys(obj, {"bindAddr", "udpPortSend", "udpPortRecv", "tcpPort"});
        HostConfig cfg;
        cfg.bindAddr = requireString(obj, "bindAddr");
        cfg.udpPortSend = requireInt(obj, "udpPortSend");
        cfg.udpPortRecv = requireInt(obj, "udpPortRecv");
        cfg.tcpPort = requireInt(obj, "tcpPort");
        return cfg;
    }

    IgConfig parseIgConfig(const JsonObject& obj)
    {
        rejectUnknownKeys(obj, {"bindAddr", "udpPortSend", "udpPortRecv", "targetAddr", "targetTcpPort",
                                "targetUdpPortRecv"});
        IgConfig cfg;
        cfg.bindAddr = requireString(obj, "bindAddr");
        cfg.udpPortSend = requireInt(obj, "udpPortSend");
        cfg.udpPortRecv = requireInt(obj, "udpPortRecv");
        cfg.targetAddr = requireString(obj, "targetAddr");
        cfg.targetTcpPort = requireInt(obj, "targetTcpPort");
        cfg.targetUdpPortRecv = requireInt(obj, "targetUdpPortRecv");
        return cfg;
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

    EngineChannelConfig parseConfig(const JsonObject& root)
    {
        rejectUnknownKeys(root, {"channelId", "offsetDeg", "igConfig", "hostConfig", "model", "window",
                                 "hostEyeStalePolicy", "requireIgConnect", "coordFrame", "entities", "entity",
                                 "camera"});

        EngineChannelConfig cfg;
        cfg.channelId = parseOptionalInt(root, "channelId", cfg.channelId);

        if (const JsonValue* v = find(root, "offsetDeg"))
            cfg.offsetDeg = parseOffsetDeg(requireObjectValue(*v, "offsetDeg"));

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

        if (find(root, "hostEyeStalePolicy") != nullptr)
            cfg.hostEyeStalePolicy = parseStalePolicy(parseOptionalString(root, "hostEyeStalePolicy", ""));

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

        const bool hasRequireIgConnect = find(root, "requireIgConnect") != nullptr;
        if (hasRequireIgConnect)
            cfg.requireIgConnect = requireBool(root, "requireIgConnect");

        validateIgEndpointPairing(cfg, hasRequireIgConnect);
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
