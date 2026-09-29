#include "dialogue_catalog.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>
#include <optional>
#include <unordered_set>

namespace mojorecomp::subtitles {
namespace {

class JsonCursor
{
public:
    explicit JsonCursor(std::string_view source) : source_(source) {}

    bool AtEnd()
    {
        SkipWhitespace();
        return position_ == source_.size();
    }

    bool Consume(char expected)
    {
        SkipWhitespace();
        if (position_ >= source_.size() || source_[position_] != expected)
            return false;
        ++position_;
        return true;
    }

    std::optional<std::string> String()
    {
        SkipWhitespace();
        if (position_ >= source_.size() || source_[position_] != '"')
            return std::nullopt;
        ++position_;

        std::string result;
        while (position_ < source_.size())
        {
            const unsigned char value = static_cast<unsigned char>(source_[position_++]);
            if (value == '"')
                return result;
            if (value < 0x20)
                return std::nullopt;
            if (value != '\\')
            {
                result.push_back(static_cast<char>(value));
                continue;
            }

            if (position_ >= source_.size())
                return std::nullopt;
            const char escaped = source_[position_++];
            switch (escaped)
            {
            case '"': result.push_back('"'); break;
            case '\\': result.push_back('\\'); break;
            case '/': result.push_back('/'); break;
            case 'b': result.push_back('\b'); break;
            case 'f': result.push_back('\f'); break;
            case 'n': result.push_back('\n'); break;
            case 'r': result.push_back('\r'); break;
            case 't': result.push_back('\t'); break;
            case 'u':
            {
                const auto codePoint = HexCodePoint();
                if (!codePoint || !AppendUtf8(result, *codePoint))
                    return std::nullopt;
                break;
            }
            default:
                return std::nullopt;
            }
        }
        return std::nullopt;
    }

    std::optional<uint64_t> Unsigned64()
    {
        SkipWhitespace();
        const size_t start = position_;
        while (position_ < source_.size() &&
               std::isdigit(static_cast<unsigned char>(source_[position_])))
            ++position_;
        if (start == position_)
            return std::nullopt;

        uint64_t value = 0;
        const auto result = std::from_chars(source_.data() + start,
                                            source_.data() + position_, value);
        if (result.ec != std::errc{} || result.ptr != source_.data() + position_)
            return std::nullopt;
        return value;
    }

    std::optional<uint32_t> Unsigned()
    {
        const auto value = Unsigned64();
        if (!value || *value > UINT32_MAX)
            return std::nullopt;
        return static_cast<uint32_t>(*value);
    }

    bool SkipValue()
    {
        SkipWhitespace();
        if (position_ >= source_.size())
            return false;
        if (source_[position_] == '"')
            return String().has_value();
        if (source_[position_] == '{')
            return SkipObject();
        if (source_[position_] == '[')
            return SkipArray();
        if (std::isdigit(static_cast<unsigned char>(source_[position_])) ||
            source_[position_] == '-')
            return SkipNumber();
        return ConsumeLiteral("true") || ConsumeLiteral("false") ||
               ConsumeLiteral("null");
    }

private:
    void SkipWhitespace()
    {
        while (position_ < source_.size() &&
               std::isspace(static_cast<unsigned char>(source_[position_])))
            ++position_;
    }

    std::optional<uint32_t> HexCodePoint()
    {
        if (position_ + 4 > source_.size())
            return std::nullopt;
        uint32_t value = 0;
        for (int index = 0; index < 4; ++index)
        {
            const char ch = source_[position_++];
            value <<= 4;
            if (ch >= '0' && ch <= '9') value |= uint32_t(ch - '0');
            else if (ch >= 'a' && ch <= 'f') value |= uint32_t(ch - 'a' + 10);
            else if (ch >= 'A' && ch <= 'F') value |= uint32_t(ch - 'A' + 10);
            else return std::nullopt;
        }
        return value;
    }

    static bool AppendUtf8(std::string& output, uint32_t codePoint)
    {
        if (codePoint >= 0xD800 && codePoint <= 0xDFFF)
            return false;
        if (codePoint <= 0x7F)
            output.push_back(static_cast<char>(codePoint));
        else if (codePoint <= 0x7FF)
        {
            output.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
            output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        }
        else if (codePoint <= 0xFFFF)
        {
            output.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
            output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
            output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        }
        else if (codePoint <= 0x10FFFF)
        {
            output.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
            output.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
            output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
            output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        }
        else
            return false;
        return true;
    }

    bool SkipObject()
    {
        if (!Consume('{'))
            return false;
        if (Consume('}'))
            return true;
        while (true)
        {
            if (!String() || !Consume(':') || !SkipValue())
                return false;
            if (Consume('}'))
                return true;
            if (!Consume(','))
                return false;
        }
    }

    bool SkipArray()
    {
        if (!Consume('['))
            return false;
        if (Consume(']'))
            return true;
        while (true)
        {
            if (!SkipValue())
                return false;
            if (Consume(']'))
                return true;
            if (!Consume(','))
                return false;
        }
    }

    bool SkipNumber()
    {
        SkipWhitespace();
        const size_t start = position_;
        if (position_ < source_.size() && source_[position_] == '-')
            ++position_;
        while (position_ < source_.size() &&
               std::isdigit(static_cast<unsigned char>(source_[position_])))
            ++position_;
        if (position_ < source_.size() && source_[position_] == '.')
        {
            ++position_;
            while (position_ < source_.size() &&
                   std::isdigit(static_cast<unsigned char>(source_[position_])))
                ++position_;
        }
        if (position_ < source_.size() &&
            (source_[position_] == 'e' || source_[position_] == 'E'))
        {
            ++position_;
            if (position_ < source_.size() &&
                (source_[position_] == '+' || source_[position_] == '-'))
                ++position_;
            while (position_ < source_.size() &&
                   std::isdigit(static_cast<unsigned char>(source_[position_])))
                ++position_;
        }
        return position_ > start;
    }

    bool ConsumeLiteral(std::string_view literal)
    {
        SkipWhitespace();
        if (source_.substr(position_, literal.size()) != literal)
            return false;
        position_ += literal.size();
        return true;
    }

    std::string_view source_;
    size_t position_ = 0;
};

bool EqualsIgnoreCase(std::string_view left, std::string_view right)
{
    if (left.size() != right.size())
        return false;
    for (size_t index = 0; index < left.size(); ++index)
    {
        if (std::tolower(static_cast<unsigned char>(left[index])) !=
            std::tolower(static_cast<unsigned char>(right[index])))
            return false;
    }
    return true;
}

bool ParseCue(JsonCursor& cursor, std::string_view asset, size_t cueIndex,
              DialogueLine& line, std::string& error)
{
    if (!cursor.Consume('{'))
    {
        error = "Dialogue cue must be a JSON object";
        return false;
    }

    bool haveStart = false;
    bool haveEnd = false;
    while (true)
    {
        if (cursor.Consume('}'))
            break;
        const auto key = cursor.String();
        if (!key || !cursor.Consume(':'))
        {
            error = "Dialogue cue contains invalid JSON";
            return false;
        }

        if (*key == "start_ms")
        {
            const auto value = cursor.Unsigned64();
            if (!value) { error = "start_ms must be an unsigned integer"; return false; }
            line.startMs = *value;
            haveStart = true;
        }
        else if (*key == "end_ms")
        {
            const auto value = cursor.Unsigned64();
            if (!value) { error = "end_ms must be an unsigned integer"; return false; }
            line.endMs = *value;
            haveEnd = true;
        }
        else if (*key == "speaker")
        {
            const auto value = cursor.String();
            if (!value) { error = "speaker must be a string"; return false; }
            line.speaker = *value;
        }
        else if (*key == "text")
        {
            const auto value = cursor.String();
            if (!value) { error = "text must be a string"; return false; }
            line.text = *value;
        }
        else if (!cursor.SkipValue())
        {
            error = "Dialogue cue contains an invalid field value";
            return false;
        }

        if (cursor.Consume('}'))
            break;
        if (!cursor.Consume(','))
        {
            error = "Dialogue cue is missing a comma";
            return false;
        }
    }

    if (!haveStart || !haveEnd || line.endMs <= line.startMs ||
        line.speaker.empty() || line.text.empty())
    {
        error = "Dialogue cue requires start_ms, end_ms, speaker, and text";
        return false;
    }

    line.asset = std::string(asset);
    line.id = line.asset + "#" + std::to_string(cueIndex + 1u);
    return true;
}

bool ParseCueArray(JsonCursor& cursor, std::string_view asset,
                   std::vector<DialogueLine>& dialogues, std::string& error)
{
    if (!cursor.Consume('['))
    {
        error = "Dialogue asset value must be an array";
        return false;
    }
    if (cursor.Consume(']'))
        return true;

    uint64_t previousStart = 0;
    bool havePrevious = false;
    size_t cueIndex = 0;
    while (true)
    {
        DialogueLine line;
        if (!ParseCue(cursor, asset, cueIndex, line, error))
            return false;
        if (havePrevious && line.startMs < previousStart)
        {
            error = "Dialogue cues must be ordered by start_ms";
            return false;
        }
        previousStart = line.startMs;
        havePrevious = true;
        dialogues.push_back(std::move(line));
        ++cueIndex;

        if (cursor.Consume(']'))
            return true;
        if (!cursor.Consume(','))
        {
            error = "Dialogue cue array is missing a comma";
            return false;
        }
    }
}

bool ParseDialogueMap(JsonCursor& cursor, DialogueCatalog& catalog,
                      std::string& error)
{
    if (!cursor.Consume('{'))
    {
        error = "dialogues must be a JSON object keyed by RSD name";
        return false;
    }
    if (cursor.Consume('}'))
        return true;

    std::unordered_set<std::string> assets;
    while (true)
    {
        const auto asset = cursor.String();
        if (!asset || asset->empty() || !cursor.Consume(':'))
        {
            error = "dialogues contains an invalid asset key";
            return false;
        }

        std::string normalized = *asset;
        std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                       [](unsigned char ch) { return char(std::tolower(ch)); });
        if (!assets.insert(normalized).second)
        {
            error = "Duplicate dialogue asset: " + *asset;
            return false;
        }
        if (!ParseCueArray(cursor, *asset, catalog.dialogues, error))
            return false;

        if (cursor.Consume('}'))
            return true;
        if (!cursor.Consume(','))
        {
            error = "dialogues object is missing a comma";
            return false;
        }
    }
}

bool ParseCatalog(std::string_view source, DialogueCatalog& catalog,
                  std::string& error)
{
    JsonCursor cursor(source);
    if (!cursor.Consume('{'))
    {
        error = "Dialogue catalog root must be a JSON object";
        return false;
    }

    bool haveDialogues = false;
    while (true)
    {
        if (cursor.Consume('}'))
            break;
        const auto key = cursor.String();
        if (!key || !cursor.Consume(':'))
        {
            error = "Dialogue catalog contains invalid JSON";
            return false;
        }

        if (*key == "schema_version")
        {
            const auto value = cursor.Unsigned();
            if (!value) { error = "schema_version must be an unsigned integer"; return false; }
            catalog.schemaVersion = *value;
        }
        else if (*key == "locale")
        {
            const auto value = cursor.String();
            if (!value) { error = "locale must be a string"; return false; }
            catalog.locale = *value;
        }
        else if (*key == "dialogues")
        {
            if (!ParseDialogueMap(cursor, catalog, error))
                return false;
            haveDialogues = true;
        }
        else if (!cursor.SkipValue())
        {
            error = "Dialogue catalog contains an invalid field value";
            return false;
        }

        if (cursor.Consume('}'))
            break;
        if (!cursor.Consume(','))
        {
            error = "Dialogue catalog is missing a comma";
            return false;
        }
    }

    if (!cursor.AtEnd())
    {
        error = "Dialogue catalog contains trailing data";
        return false;
    }
    if (catalog.schemaVersion != 1)
    {
        error = "Unsupported dialogue catalog schema_version";
        return false;
    }
    if (catalog.locale.empty())
    {
        error = "Dialogue catalog locale cannot be empty";
        return false;
    }
    if (!haveDialogues)
    {
        error = "Dialogue catalog is missing dialogues";
        return false;
    }
    return true;
}

} // namespace

const DialogueLine* DialogueCatalog::Find(std::string_view id) const noexcept
{
    for (const auto& dialogue : dialogues)
        if (dialogue.id == id)
            return &dialogue;
    return nullptr;
}

const DialogueLine* DialogueCatalog::FindCue(std::string_view asset,
                                              uint64_t timeMs) const noexcept
{
    for (const auto& dialogue : dialogues)
    {
        if (!EqualsIgnoreCase(dialogue.asset, asset))
            continue;
        if (timeMs >= dialogue.startMs && timeMs < dialogue.endMs)
            return &dialogue;
    }
    return nullptr;
}

bool DialogueCatalog::HasAsset(std::string_view asset) const noexcept
{
    for (const auto& dialogue : dialogues)
        if (EqualsIgnoreCase(dialogue.asset, asset))
            return true;
    return false;
}

bool LoadDialogueCatalog(const std::filesystem::path& path,
                         DialogueCatalog& catalog,
                         std::string& error)
{
    catalog = {};
    error.clear();

    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        error = "Could not open dialogue catalog: " + path.string();
        return false;
    }
    input.seekg(0, std::ios::end);
    const auto length = input.tellg();
    if (length <= 0 || length > std::streamoff(16 * 1024 * 1024))
    {
        error = "Dialogue catalog has an invalid size";
        return false;
    }
    std::string source(static_cast<size_t>(length), '\0');
    input.seekg(0, std::ios::beg);
    input.read(source.data(), length);
    if (!input)
    {
        error = "Could not read dialogue catalog: " + path.string();
        return false;
    }

    DialogueCatalog parsed;
    if (!ParseCatalog(source, parsed, error))
        return false;
    catalog = std::move(parsed);
    return true;
}

} // namespace mojorecomp::subtitles
