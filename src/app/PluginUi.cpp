#include "PluginUi.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QSpinBox>
#include <QTextBrowser>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>
#include <functional>

namespace {

QColor fromPacked(double v) {
    const auto rgba = static_cast<std::uint32_t>(std::llround(v));
    return QColor(static_cast<int>(rgba >> 24 & 255), static_cast<int>(rgba >> 16 & 255),
                  static_cast<int>(rgba >> 8 & 255), static_cast<int>(rgba & 255));
}

double toPacked(const QColor& c) {
    return static_cast<double>((static_cast<std::uint32_t>(c.red()) << 24) |
                               (static_cast<std::uint32_t>(c.green()) << 16) |
                               (static_cast<std::uint32_t>(c.blue()) << 8) | static_cast<std::uint32_t>(c.alpha()));
}

void paintSwatch(QToolButton* b, const QColor& c) {
    QPixmap pm(36, 18);
    pm.fill(Qt::white);
    QPainter p(&pm);
    p.fillRect(pm.rect(), c);
    p.setPen(Qt::black);
    p.drawRect(pm.rect().adjusted(0, 0, -1, -1));
    p.end();
    b->setIcon(QIcon(pm));
    b->setIconSize(pm.size());
}

} // namespace

bool editFilterParams(QWidget* parent, const gas::PluginFilter& filter, std::vector<double>& values) {
    values.resize(filter.params.size());
    QDialog d(parent);
    d.setWindowTitle(QString::fromStdString(filter.name));
    auto* form = new QFormLayout;
    std::vector<std::function<double()>> readers;
    for (std::size_t i = 0; i < filter.params.size(); ++i) {
        const gas::PluginParam& p = filter.params[i];
        const QString label = QString::fromStdString(p.label);
        const double v = values[i];
        switch (p.type) {
        case GAS_PARAM_INT: {
            auto* box = new QSpinBox(&d);
            box->setRange(static_cast<int>(std::ceil(p.minValue)), static_cast<int>(std::floor(p.maxValue)));
            box->setValue(static_cast<int>(std::lround(v)));
            form->addRow(label, box);
            readers.push_back([box] { return static_cast<double>(box->value()); });
            break;
        }
        case GAS_PARAM_BOOL: {
            auto* box = new QCheckBox(label, &d);
            box->setChecked(v != 0);
            form->addRow(QString(), box);
            readers.push_back([box] { return box->isChecked() ? 1.0 : 0.0; });
            break;
        }
        case GAS_PARAM_COLOR: {
            auto* button = new QToolButton(&d);
            auto colour = std::make_shared<QColor>(fromPacked(v));
            paintSwatch(button, *colour);
            QObject::connect(button, &QToolButton::clicked, &d, [button, colour, label, &d] {
                const QColor chosen = QColorDialog::getColor(*colour, &d, label, QColorDialog::ShowAlphaChannel);
                if (chosen.isValid()) {
                    *colour = chosen;
                    paintSwatch(button, chosen);
                }
            });
            form->addRow(label, button);
            readers.push_back([colour] { return toPacked(*colour); });
            break;
        }
        case GAS_PARAM_CHOICE: {
            auto* box = new QComboBox(&d);
            for (const std::string& c : p.choices) {
                box->addItem(QString::fromStdString(c));
            }
            box->setCurrentIndex(static_cast<int>(std::lround(v)));
            form->addRow(label, box);
            readers.push_back([box] { return static_cast<double>(box->currentIndex()); });
            break;
        }
        case GAS_PARAM_FLOAT:
        default: {
            auto* box = new QDoubleSpinBox(&d);
            box->setDecimals(3);
            box->setRange(p.minValue, p.maxValue);
            box->setSingleStep(std::max(0.001, (p.maxValue - p.minValue) / 100.0));
            box->setValue(v);
            form->addRow(label, box);
            readers.push_back([box] { return box->value(); });
            break;
        }
        }
    }
    auto* layout = new QVBoxLayout(&d);
    if (!filter.description.empty()) {
        auto* about = new QLabel(QString::fromStdString(filter.description), &d);
        about->setWordWrap(true);
        layout->addWidget(about);
    }
    layout->addLayout(form);
    auto* source = new QLabel(QObject::tr("From plugin: %1").arg(QString::fromStdString(filter.plugin)), &d);
    source->setEnabled(false);
    layout->addWidget(source);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    layout->addWidget(buttons);
    if (d.exec() != QDialog::Accepted) {
        return false;
    }
    for (std::size_t i = 0; i < readers.size(); ++i) {
        values[i] = readers[i]();
    }
    return true;
}

void showPluginInfo(QWidget* parent, const gas::PluginHost& host, const QString& userFolder) {
    QString html = QObject::tr("<p>Plugin API version %1. Plugins are loaded from the <b>plugins</b> folder next to "
                               "the program and from<br><code>%2</code></p>")
                       .arg(GAS_PLUGIN_API_VERSION)
                       .arg(userFolder.toHtmlEscaped());
    if (host.plugins().empty()) {
        html += QObject::tr("<p><i>No plugins found.</i></p>");
    }
    for (const gas::PluginInfo& p : host.plugins()) {
        html += QStringLiteral("<h3>%1</h3>").arg(QString::fromStdString(p.name).toHtmlEscaped());
        if (!p.file.empty()) {
            html += QStringLiteral("<p><code>%1</code></p>")
                        .arg(QString::fromStdWString(p.file.wstring()).toHtmlEscaped());
        }
        if (p.loaded) {
            html += QObject::tr("<p>Loaded: %1 filter(s), %2 exporter(s)</p>").arg(p.filters).arg(p.exporters);
        } else {
            html += QObject::tr("<p style='color:#d04040'>Not loaded: %1</p>")
                        .arg(QString::fromStdString(p.error).toHtmlEscaped());
        }
        if (!p.log.empty()) {
            html += QStringLiteral("<ul>");
            for (const std::string& line : p.log) {
                html += QStringLiteral("<li>%1</li>").arg(QString::fromStdString(line).toHtmlEscaped());
            }
            html += QStringLiteral("</ul>");
        }
    }
    QDialog d(parent);
    d.setWindowTitle(QObject::tr("Plugins"));
    auto* text = new QTextBrowser(&d);
    text->setHtml(html);
    text->setMinimumSize(520, 320);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &d);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    auto* layout = new QVBoxLayout(&d);
    layout->addWidget(text);
    layout->addWidget(buttons);
    d.exec();
}
