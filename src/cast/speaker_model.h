#pragma once

#include "cast/speaker_menu.h"

#include <QAbstractListModel>
#include <QString>

#include <vector>

/// @brief One speaker as the picker lists it
struct SpeakerRow {
    /// What CastEngine's calls take: the speaker's client_id, or its URL until it has connected
    QString id;
    QString name;
    bool in_group{false};
    bool plays{false};
    bool paired{false};
    /// It offers a pairing method
    bool can_pair{false};
    speaker_menu::Row row;

    bool operator==(const SpeakerRow&) const = default;
};

/// @brief The speakers found or added, for the picker and Settings to bind to. CastEngine fills
/// it; it holds no logic of its own.
class SpeakerModel : public QAbstractListModel {
    Q_OBJECT

public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        NameRole,
        InGroupRole,
        PlaysRole,
        /// speaker_menu::Check, as an int
        CheckRole,
        /// speaker_menu::Click, as an int
        ClickRole,
        /// speaker_menu::Status, as an int
        StatusRole,
        /// The status in words; empty while there is nothing to say
        StatusTextRole,
        WarningRole,
        PairedRole,
        CanPairRole,
    };
    Q_ENUM(Role)

    using QAbstractListModel::QAbstractListModel;

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    const std::vector<SpeakerRow>& rows() const {
        return this->rows_;
    }

    /// @brief Replaces the rows, telling views only what changed
    void set_rows(std::vector<SpeakerRow> rows);

    static QString status_text(speaker_menu::Status status);

private:
    std::vector<SpeakerRow> rows_;
};
