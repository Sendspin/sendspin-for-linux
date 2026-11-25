#include "cast/speaker_model.h"

#include <algorithm>
#include <utility>

using speaker_menu::Status;

int SpeakerModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(this->rows_.size());
}

QVariant SpeakerModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= static_cast<int>(this->rows_.size())) {
        return {};
    }
    const SpeakerRow& row = this->rows_[static_cast<size_t>(index.row())];
    switch (role) {
        case Qt::DisplayRole:
        case NameRole:
            return row.name;
        case IdRole:
            return row.id;
        case InGroupRole:
            return row.in_group;
        case PlaysRole:
            return row.plays;
        case CheckRole:
            return static_cast<int>(row.row.check);
        case ClickRole:
            return static_cast<int>(row.row.click);
        case StatusRole:
            return static_cast<int>(row.row.status);
        case StatusTextRole:
            return status_text(row.row.status);
        case WarningRole:
            return row.row.warning;
        case PairedRole:
            return row.paired;
        case CanPairRole:
            return row.can_pair;
        default:
            return {};
    }
}

QHash<int, QByteArray> SpeakerModel::roleNames() const {
    return {
        {IdRole, "speakerId"},    {NameRole, "name"},
        {InGroupRole, "inGroup"}, {PlaysRole, "plays"},
        {CheckRole, "check"},     {ClickRole, "click"},
        {StatusRole, "status"},   {StatusTextRole, "statusText"},
        {WarningRole, "warning"}, {PairedRole, "paired"},
        {CanPairRole, "canPair"},
    };
}

void SpeakerModel::set_rows(std::vector<SpeakerRow> rows) {
    const bool same_speakers =
        std::ranges::equal(rows, this->rows_, {}, &SpeakerRow::id, &SpeakerRow::id);
    if (!same_speakers) {
        this->beginResetModel();
        this->rows_ = std::move(rows);
        this->endResetModel();
        return;
    }
    for (size_t i = 0; i < rows.size(); i++) {
        if (rows[i] != this->rows_[i]) {
            this->rows_[i] = std::move(rows[i]);
            const QModelIndex index = this->index(static_cast<int>(i));
            emit this->dataChanged(index, index);
        }
    }
}

QString SpeakerModel::status_text(Status status) {
    switch (status) {
        case Status::NONE:
            return {};
        case Status::CONNECTING:
            return tr("Connecting…");
        case Status::UNAVAILABLE:
            return tr("Unavailable");
        case Status::IN_USE_ELSEWHERE:
            return tr("In use elsewhere");
        case Status::DISCONNECTED:
            return tr("Disconnected");
        case Status::CANT_CONNECT:
            return tr("Can't connect");
        case Status::CANT_PLAY:
            return tr("Can't play");
        case Status::NEEDS_PAIRING:
            return tr("Needs pairing");
        case Status::NEEDS_PAIRING_AGAIN:
            return tr("Needs pairing again");
        case Status::IDENTITY_CHANGED:
            return tr("Identity changed");
    }
    return {};
}
