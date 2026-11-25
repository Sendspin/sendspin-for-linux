// The registry walk started from sendspin-cpp-cli's src/pipewire_sink.cpp at 715f988
// (Apache-2.0, https://github.com/Sendspin/sendspin-cpp-cli).

#include "audio/outputs.h"

#include "audio/pipewire_util.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>

#include <pipewire/extensions/metadata.h>
#include <spa/param/audio/raw.h>
#include <spa/param/format-utils.h>
#include <spa/pod/iter.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <utility>

Q_LOGGING_CATEGORY(lc_outputs, "sendspin.outputs")

namespace {

constexpr int ROUND_TRIP_TIMEOUT_MS = 1000;
constexpr auto RETRY_INTERVAL = std::chrono::seconds(2);

bool over_16_bit(uint32_t format) {
    switch (format) {
        case SPA_AUDIO_FORMAT_S24_LE:
        case SPA_AUDIO_FORMAT_S24_BE:
        case SPA_AUDIO_FORMAT_S24_32_LE:
        case SPA_AUDIO_FORMAT_S24_32_BE:
        case SPA_AUDIO_FORMAT_S32_LE:
        case SPA_AUDIO_FORMAT_S32_BE:
        case SPA_AUDIO_FORMAT_F32_LE:
        case SPA_AUDIO_FORMAT_F32_BE:
        case SPA_AUDIO_FORMAT_F64_LE:
        case SPA_AUDIO_FORMAT_F64_BE:
        case SPA_AUDIO_FORMAT_S24P:
        case SPA_AUDIO_FORMAT_S24_32P:
        case SPA_AUDIO_FORMAT_S32P:
        case SPA_AUDIO_FORMAT_F32P:
        case SPA_AUDIO_FORMAT_F64P:
            return true;
        default:
            return false;
    }
}

/// Adds one EnumFormat entry: raw audio's sample formats and rates, each one value or a choice
void add_format(SinkFormats& formats, const spa_pod* param) {
    uint32_t media_type = 0;
    uint32_t media_subtype = 0;
    if (spa_format_parse(param, &media_type, &media_subtype) < 0 ||
        media_type != SPA_MEDIA_TYPE_audio || media_subtype != SPA_MEDIA_SUBTYPE_raw) {
        return;
    }
    uint32_t count = 0;
    uint32_t choice = SPA_CHOICE_None;
    if (const spa_pod_prop* prop = spa_pod_find_prop(param, nullptr, SPA_FORMAT_AUDIO_format)) {
        const spa_pod* values = spa_pod_get_values(&prop->value, &count, &choice);
        if (values->type == SPA_TYPE_Id) {
            const auto* ids = static_cast<const uint32_t*>(SPA_POD_BODY_CONST(values));
            formats.over_16_bit = formats.over_16_bit || std::any_of(ids, ids + count, over_16_bit);
        }
    }
    const spa_pod_prop* prop = spa_pod_find_prop(param, nullptr, SPA_FORMAT_AUDIO_rate);
    if (prop == nullptr) {
        return;
    }
    const spa_pod* values = spa_pod_get_values(&prop->value, &count, &choice);
    if (values->type != SPA_TYPE_Int || count == 0) {
        return;
    }
    const auto* rates = static_cast<const int32_t*>(SPA_POD_BODY_CONST(values));
    if (choice == SPA_CHOICE_Range || choice == SPA_CHOICE_Step) {
        // The default, then the bounds
        if (count >= 3 && rates[1] > 0 && rates[2] >= rates[1]) {
            const auto low = static_cast<uint32_t>(rates[1]);
            const auto high = static_cast<uint32_t>(rates[2]);
            formats.rate_min = formats.rate_max == 0 ? low : std::min(formats.rate_min, low);
            formats.rate_max = std::max(formats.rate_max, high);
        }
        return;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const auto rate = static_cast<uint32_t>(std::max(rates[i], 0));
        if (rate != 0 &&
            std::find(formats.rates.begin(), formats.rates.end(), rate) == formats.rates.end()) {
            formats.rates.push_back(rate);
        }
    }
}

bool exists(const std::vector<OutputSink>& sinks, const QByteArray& name) {
    return !name.isEmpty() && std::any_of(sinks.begin(), sinks.end(),
                                          [&name](const auto& sink) { return sink.name == name; });
}

}  // namespace

std::optional<QByteArray> choose_target(const std::vector<OutputSink>& sinks,
                                        const QByteArray& default_sink, const QByteArray& chosen,
                                        const QByteArray& own, const QByteArray& last_default) {
    for (const QByteArray& wanted : {chosen, default_sink, last_default}) {
        if (wanted != own && exists(sinks, wanted)) {
            return wanted;
        }
    }
    const OutputSink* best = nullptr;
    for (const OutputSink& sink : sinks) {
        if (sink.name != own && (best == nullptr || sink.priority > best->priority)) {
            best = &sink;
        }
    }
    return best != nullptr ? std::optional(best->name) : std::nullopt;
}

Outputs::Outputs(QByteArray own_sink, QObject* parent)
    : QObject(parent), own_sink_(std::move(own_sink)) {
    pw_init(nullptr, nullptr);
    this->retry_.setInterval(RETRY_INTERVAL);
    connect(&this->retry_, &QTimer::timeout, this, &Outputs::start);
}

Outputs::~Outputs() {
    this->stop();
    pw_deinit();
}

bool Outputs::start() {
    this->stop();

    // Static: the listeners keep the pointers. Zeroed then assigned: the structs grow.
    static constexpr pw_core_events core_events = [] {
        pw_core_events table{};
        table.version = PW_VERSION_CORE_EVENTS;
        table.done = &Outputs::on_core_done;
        table.error = &Outputs::on_core_error;
        return table;
    }();
    static constexpr pw_registry_events registry_events = [] {
        pw_registry_events table{};
        table.version = PW_VERSION_REGISTRY_EVENTS;
        table.global = &Outputs::on_global;
        table.global_remove = &Outputs::on_global_remove;
        return table;
    }();

    bool listed = false;
    this->loop_ = pw_thread_loop_new("sendspin-outputs", nullptr);
    if (this->loop_ != nullptr && pw_thread_loop_start(this->loop_) >= 0) {
        const LoopLock lock(this->loop_);
        this->context_ = pw_context_new(pw_thread_loop_get_loop(this->loop_), nullptr, 0);
        if (this->context_ != nullptr) {
            this->core_ = pw_context_connect(this->context_, nullptr, 0);
        }
        if (this->core_ != nullptr) {
            pw_core_add_listener(this->core_, &this->core_hook_, &core_events, this);
            this->registry_ = pw_core_get_registry(this->core_, PW_VERSION_REGISTRY, 0);
        }
        if (this->registry_ != nullptr) {
            pw_registry_add_listener(this->registry_, &this->registry_hook_, &registry_events,
                                     this);
            listed = this->sync_(ROUND_TRIP_TIMEOUT_MS);
        }
    }
    if (!listed) {
        this->stop();
        this->retry_.start();
        return false;
    }
    this->retry_.stop();
    this->update_target_();
    return true;
}

void Outputs::stop() {
    this->retry_.stop();
    if (this->loop_ != nullptr) {
        {
            const LoopLock lock(this->loop_);
            unbind_metadata_(this->default_metadata_);
            unbind_metadata_(this->settings_metadata_);
            if (this->registry_ != nullptr) {
                spa_hook_remove(&this->registry_hook_);
                pw_proxy_destroy(reinterpret_cast<pw_proxy*>(this->registry_));
                this->registry_ = nullptr;
            }
            if (this->core_ != nullptr) {
                spa_hook_remove(&this->core_hook_);
                pw_core_disconnect(this->core_);
                this->core_ = nullptr;
            }
            if (this->context_ != nullptr) {
                pw_context_destroy(this->context_);
                this->context_ = nullptr;
            }
        }
        pw_thread_loop_destroy(this->loop_);
        this->loop_ = nullptr;
    }
    // last_default_ stays: the outputs come back as they were
    this->sinks_.clear();
    this->default_sink_.clear();
    this->clock_ = {};
}

void Outputs::set_chosen(const QByteArray& name) {
    this->chosen_ = name;
    this->update_target_();
}

ClockRates Outputs::clock() const {
    if (this->loop_ == nullptr) {
        return {};
    }
    const LoopLock lock(this->loop_);
    return this->clock_;
}

SinkFormats Outputs::formats(const QByteArray& sink) {
    static constexpr pw_node_events node_events = [] {
        pw_node_events table{};
        table.version = PW_VERSION_NODE_EVENTS;
        table.param = &Outputs::on_node_param;
        return table;
    }();

    if (this->loop_ == nullptr) {
        return {};
    }
    const LoopLock lock(this->loop_);
    const auto found = std::find_if(this->sinks_.begin(), this->sinks_.end(),
                                    [&sink](const auto& known) { return known.name == sink; });
    if (found == this->sinks_.end()) {
        return {};
    }
    auto* node = static_cast<pw_proxy*>(
        pw_registry_bind(this->registry_, found->id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0));
    if (node == nullptr) {
        return {};
    }
    SinkFormats formats;
    spa_hook hook{};
    pw_node_add_listener(reinterpret_cast<pw_node*>(node), &hook, &node_events, this);
    this->asked_ = &formats;
    pw_node_enum_params(reinterpret_cast<pw_node*>(node), 0, SPA_PARAM_EnumFormat, 0, UINT32_MAX,
                        nullptr);
    const bool answered = this->sync_(ROUND_TRIP_TIMEOUT_MS);
    this->asked_ = nullptr;
    spa_hook_remove(&hook);
    pw_proxy_destroy(node);
    return answered ? formats : SinkFormats{};
}

bool Outputs::sync_(int timeout_ms) {
    this->synced_ = false;
    this->sync_seq_ = pw_core_sync(this->core_, PW_ID_CORE, 0);
    timespec deadline{};
    pw_thread_loop_get_time(this->loop_, &deadline, static_cast<int64_t>(timeout_ms) * 1000 * 1000);
    while (!this->synced_) {
        if (pw_thread_loop_timed_wait_full(this->loop_, &deadline) != 0) {
            break;
        }
    }
    return this->synced_;
}

void Outputs::changed_() {
    QMetaObject::invokeMethod(this, &Outputs::update_target_, Qt::QueuedConnection);
}

void Outputs::update_target_() {
    std::optional<QByteArray> target;
    std::vector<OutputSink> choices;
    if (this->loop_ != nullptr) {
        const LoopLock lock(this->loop_);
        target = choose_target(this->sinks_, this->default_sink_, this->chosen_, this->own_sink_,
                               this->last_default_);
        choices = this->sinks_;
    }
    std::erase_if(choices, [this](const auto& sink) { return sink.name == this->own_sink_; });
    std::ranges::stable_sort(choices, std::ranges::greater{}, &OutputSink::priority);
    if (choices != this->choices_) {
        this->choices_ = std::move(choices);
        emit this->choicesChanged();
    }
    if (target != this->target_) {
        this->target_ = target;
        qCInfo(lc_outputs, "playing on %s", target ? target->constData() : "nothing");
        emit this->targetChanged();
    }
}

void Outputs::lost_() {
    if (this->loop_ == nullptr) {
        return;
    }
    qCWarning(lc_outputs, "PipeWire went away");
    this->stop();
    this->update_target_();
    this->retry_.start();
}

void Outputs::bind_metadata_(Metadata& metadata, uint32_t id, bool settings) {
    static constexpr pw_metadata_events default_events = [] {
        pw_metadata_events table{};
        table.version = PW_VERSION_METADATA_EVENTS;
        table.property = &Outputs::on_default;
        return table;
    }();
    static constexpr pw_metadata_events settings_events = [] {
        pw_metadata_events table{};
        table.version = PW_VERSION_METADATA_EVENTS;
        table.property = &Outputs::on_setting;
        return table;
    }();

    if (metadata.proxy != nullptr) {
        return;
    }
    metadata.proxy = static_cast<pw_proxy*>(
        pw_registry_bind(this->registry_, id, PW_TYPE_INTERFACE_Metadata, PW_VERSION_METADATA, 0));
    if (metadata.proxy == nullptr) {
        return;
    }
    metadata.id = id;
    pw_metadata_add_listener(reinterpret_cast<pw_metadata*>(metadata.proxy), &metadata.hook,
                             settings ? &settings_events : &default_events, this);
    // Its values follow the bind: whoever waits for the listing waits for them too
    this->synced_ = false;
    this->sync_seq_ = pw_core_sync(this->core_, PW_ID_CORE, 0);
}

void Outputs::unbind_metadata_(Metadata& metadata) {
    if (metadata.proxy != nullptr) {
        spa_hook_remove(&metadata.hook);
        pw_proxy_destroy(metadata.proxy);
        metadata = {};
    }
}

void Outputs::on_global(void* data, uint32_t id, uint32_t /*permissions*/, const char* type,
                        uint32_t /*version*/, const spa_dict* props) {
    auto* self = static_cast<Outputs*>(data);
    if (props == nullptr || type == nullptr) {
        return;
    }
    if (std::strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0) {
        const char* name = spa_dict_lookup(props, PW_KEY_METADATA_NAME);
        if (name != nullptr && std::strcmp(name, "default") == 0) {
            self->bind_metadata_(self->default_metadata_, id, false);
        } else if (name != nullptr && std::strcmp(name, "settings") == 0) {
            self->bind_metadata_(self->settings_metadata_, id, true);
        }
        return;
    }
    if (std::strcmp(type, PW_TYPE_INTERFACE_Node) != 0) {
        return;
    }
    const char* media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
    const char* name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    if (media_class == nullptr || name == nullptr || std::strcmp(media_class, "Audio/Sink") != 0) {
        return;
    }
    const char* priority = spa_dict_lookup(props, PW_KEY_PRIORITY_SESSION);
    const char* description = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
    self->sinks_.push_back({id, name, priority != nullptr ? std::atoi(priority) : 0,
                            QString::fromUtf8(description != nullptr ? description : name)});
    self->changed_();
}

void Outputs::on_global_remove(void* data, uint32_t id) {
    auto* self = static_cast<Outputs*>(data);
    if (std::erase_if(self->sinks_, [id](const auto& sink) { return sink.id == id; }) != 0) {
        self->changed_();
    }
    // The session manager went, and its metadata with it: the one it comes back with is new
    if (self->default_metadata_.proxy != nullptr && self->default_metadata_.id == id) {
        unbind_metadata_(self->default_metadata_);
        self->default_sink_.clear();
        self->changed_();
    } else if (self->settings_metadata_.proxy != nullptr && self->settings_metadata_.id == id) {
        unbind_metadata_(self->settings_metadata_);
    }
}

void Outputs::on_core_done(void* data, uint32_t id, int seq) {
    auto* self = static_cast<Outputs*>(data);
    if (id == PW_ID_CORE && seq == self->sync_seq_) {
        self->synced_ = true;
        pw_thread_loop_signal(self->loop_, false);
    }
}

void Outputs::on_core_error(void* data, uint32_t id, int /*seq*/, int res,
                            const char* /*message*/) {
    auto* self = static_cast<Outputs*>(data);
    if (id == PW_ID_CORE && res == -EPIPE) {
        QMetaObject::invokeMethod(self, &Outputs::lost_, Qt::QueuedConnection);
    }
}

int Outputs::on_default(void* data, uint32_t /*subject*/, const char* key, const char* /*type*/,
                        const char* value) {
    auto* self = static_cast<Outputs*>(data);
    // No key: everything was removed
    if (key != nullptr && std::strcmp(key, "default.audio.sink") != 0) {
        return 0;
    }
    // {"name": "<node.name>"}
    self->default_sink_ =
        (key == nullptr || value == nullptr)
            ? QByteArray()
            : QJsonDocument::fromJson(value).object().value("name").toString().toUtf8();
    if (!self->default_sink_.isEmpty() && self->default_sink_ != self->own_sink_) {
        self->last_default_ = self->default_sink_;
    }
    self->changed_();
    return 0;
}

int Outputs::on_setting(void* data, uint32_t /*subject*/, const char* key, const char* /*type*/,
                        const char* value) {
    auto* self = static_cast<Outputs*>(data);
    if (key == nullptr || value == nullptr) {
        return 0;
    }
    if (std::strcmp(key, "clock.rate") == 0) {
        if (const int rate = std::atoi(value); rate > 0) {
            self->clock_.rate = static_cast<uint32_t>(rate);
        }
    } else if (std::strcmp(key, "clock.allowed-rates") == 0) {
        self->clock_.allowed = parse_rates(value);
    } else if (std::strcmp(key, "clock.force-rate") == 0) {
        self->clock_.forced = static_cast<uint32_t>(std::max(std::atoi(value), 0));
    }
    return 0;
}

void Outputs::on_node_param(void* data, int /*seq*/, uint32_t id, uint32_t /*index*/,
                            uint32_t /*next*/, const spa_pod* param) {
    auto* self = static_cast<Outputs*>(data);
    if (id == SPA_PARAM_EnumFormat && param != nullptr && self->asked_ != nullptr) {
        add_format(*self->asked_, param);
    }
}
