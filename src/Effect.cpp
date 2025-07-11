/*
 *   Copyright © 2015 Robert Metsäranta <therealestrob@gmail.com>
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *   General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; see the file COPYING.  if not, write to
 *   the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 *   Boston, MA 02110-1301, USA.
 */

#include "Effect.h"
#include "Config.h"

#include <QtDBus/QDBusConnection>
#include <QDBusError>
#include <QJsonDocument>
#include <QJsonArray>
#include <KX11Extras>

#if QT_VERSION_MAJOR >= 6
    #include <opengl/glutils.h>
    #include <effect/effectwindow.h>
    #include <effect/effecthandler.h>
    #include <core/renderviewport.h>
#else
    #include <kwineffects.h>
    #include <kwinglutils.h>
#endif


ShapeCorners::Effect::Effect()
    : KWin::OffscreenEffect()
{
    reconfigure(ReconfigureAll);

    if (auto connection = QDBusConnection::sessionBus();
        !connection.isConnected()) {
        qWarning() << "ShapeCorners: Cannot connect to the D-Bus session bus.";
    }
    else {
        if (!connection.registerService(QStringLiteral("org.kde.ShapeCorners"))) {
            qWarning() << "ShapeCorners:" << connection.lastError().message();
        }
        else {
            if (!connection.registerObject(QStringLiteral("/ShapeCornersEffect"), this, QDBusConnection::ExportAllSlots)) {
                qWarning() << "ShapeCorners:" << connection.lastError().message();
            }
        }
    }

    if(m_shaderManager.IsValid()) {
        for (const auto& win: KWin::effects->stackingOrder())
            windowAdded(win);
        connect(KWin::effects, &KWin::EffectsHandler::windowAdded, this, &Effect::windowAdded);
        connect(KWin::effects, &KWin::EffectsHandler::windowDeleted, this, &Effect::windowRemoved);
    }
}

ShapeCorners::Effect::~Effect() = default;

void
ShapeCorners::Effect::windowAdded(KWin::EffectWindow *w)
{
    // Don't treat docks as windows. They are needed for the maximized check only.
    if (w->isDock()) {
#ifdef QT_DEBUG
    qInfo() << "ShapeCorners: menu added." << w;
#endif
        m_menuBars.push_back(w);
        return;
    }

#ifdef QT_DEBUG
    qInfo() << "ShapeCorners: window added." << w;
#endif

    if (w->windowClass().trimmed().isEmpty() && w->caption().trimmed().isEmpty()) {
#ifdef QT_DEBUG
        qWarning() << "ShapeCorners: window does not have a valid class name.";
#endif
        return;
    }

    const QSet hardExceptions {
        QStringLiteral("kwin"),
        QStringLiteral("kwin_x11"),
        QStringLiteral("kwin_wayland"),
        QStringLiteral("kscreenlocker_greet"),
        QStringLiteral("ksmserver"),
        QStringLiteral("krunner"),
        QStringLiteral("ksplashqml"),
        // QStringLiteral("plasmashell"),        Note: Don't add it to exceptions, it involves widget config windows
    };
    const auto name = w->windowClass().split(QChar::Space).first();
    if (hardExceptions.contains(name)) {
#ifdef QT_DEBUG
        qWarning() << "ShapeCorners: ignoring window explicitly.";
#endif
        return;
    }

    auto window = new Window(*w);
    auto pair = std::make_pair(w, window);
    if (const auto& [iter, r] = m_managed.insert(pair); !r) {
#ifdef QT_DEBUG
        qWarning() << "ShapeCorners: ignoring duplicate window.";
#endif
        return;
    }

#if QT_VERSION_MAJOR >= 6
    connect(w, &KWin::EffectWindow::windowFrameGeometryChanged, this, &Effect::windowResized);
#else
    connect(KWin::effects, &KWin::EffectsHandler::windowFrameGeometryChanged, this, &Effect::windowResized);
#endif
    connect(Config::self(), &Config::configChanged, window, &Window::configChanged);
    redirect(w);
    setShader(w, m_shaderManager.GetShader().get());
    checkTiled();
    checkMaximized(w);
}

void ShapeCorners::Effect::windowRemoved(KWin::EffectWindow *w)
{
    auto window_iterator = m_managed.find(w);
    if (window_iterator != m_managed.end()) {
        qDebug() << "ShapeCorners: window removed" << window_iterator->first->windowClass();
        window_iterator->second->deleteLater();
        m_managed.erase(window_iterator);
    } else {
        auto menubar_iterator = std::find(m_menuBars.begin(), m_menuBars.end(), w);
        if (menubar_iterator != m_menuBars.end()) {
            qDebug() << "ShapeCorners: menu removed" << w->windowClass();
            m_menuBars.erase(menubar_iterator);
        } else {
            qDebug() << "ShapeCorners: window removed";
        }
    }
    checkTiled();
}

void
ShapeCorners::Effect::reconfigure(const ReconfigureFlags flags)
{
    Q_UNUSED(flags)
    Config::self()->read();
}

void ShapeCorners::Effect::prePaintWindow(KWin::EffectWindow *w, KWin::WindowPrePaintData &data, std::chrono::milliseconds time)
{
    auto window_iterator = m_managed.find(w);
    if (!m_shaderManager.IsValid()
        || window_iterator == m_managed.end()
        || !window_iterator->second->hasEffect())
    {
        OffscreenEffect::prePaintWindow(w, data, time);
        return;
    }

    m_time = time;
    window_iterator->second->animateProperties(time);

    if(window_iterator->second->hasRoundCorners()) {
#if QT_VERSION_MAJOR >= 6
        const auto geo = w->frameGeometry() * w->screen()->scale();
        const auto size = window_iterator->second->cornerRadius * w->screen()->scale();
#else
        const auto geo = w->frameGeometry() * KWin::effects->renderTargetScale();
        const auto size = window_iterator->second->cornerRadius * KWin::effects->renderTargetScale();
#endif

        QRegion reg{};
        reg += QRect(geo.x(), geo.y(), size, size);
        reg += QRect(geo.x() + geo.width() - size, geo.y(), size, size);
        reg += QRect(geo.x(), geo.y() + geo.height() - size, size, size);
        reg += QRect(geo.x() + geo.width() - size, geo.y() + geo.height() - size, size, size);
        data.opaque -= reg;
        data.paint += reg;
        data.setTranslucent();
    }

    OffscreenEffect::prePaintWindow(w, data, time);

    // Force continuous repainting for animation
    w->addRepaintFull();
}

bool ShapeCorners::Effect::supported()
{
    return KWin::effects->isOpenGLCompositing();
}

#if QT_VERSION_MAJOR >= 6
void ShapeCorners::Effect::drawWindow(const KWin::RenderTarget &renderTarget, const KWin::RenderViewport &viewport,
                                    KWin::EffectWindow *w, int mask, const QRegion &region,
                                    KWin::WindowPaintData &data) {
#else
void ShapeCorners::Effect::drawWindow(KWin::EffectWindow *w, int mask, const QRegion &region,
                                    KWin::WindowPaintData &data) {
#endif
    auto window_iterator = m_managed.find(w);
    if (!m_shaderManager.IsValid()
        || window_iterator == m_managed.end()
        || !window_iterator->second->hasEffect())
    {
        unredirect(w);
#if QT_VERSION_MAJOR >= 6
        OffscreenEffect::drawWindow(renderTarget, viewport, w, mask, region, data);
#else
        OffscreenEffect::drawWindow(w, mask, region, data);
#endif
        return;
    }

#if QT_VERSION_MAJOR >= 6
    const auto scale = viewport.scale();
#else
    const auto scale = KWin::effects->renderTargetScale();
#endif

    redirect(w);
    setShader(w, m_shaderManager.GetShader().get());
    m_shaderManager.Bind(*window_iterator->second, scale, m_time);
    glActiveTexture(GL_TEXTURE0);

#if QT_VERSION_MAJOR >= 6
    OffscreenEffect::drawWindow(renderTarget, viewport, w, mask, region, data);
#else
    OffscreenEffect::drawWindow(w, mask, region, data);
#endif
    m_shaderManager.Unbind();
}

QString ShapeCorners::Effect::get_window_titles() const {
    QJsonArray array;
    for (const auto& [w, window]: m_managed) {
        auto json = window->toJson();
        if (!array.contains(json))
            array.push_back(json);
    }
    auto doc = QJsonDocument(array).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(doc);
}

void ShapeCorners::Effect::checkTiled() {
    TileChecker tileChecker (m_managed);
    tileChecker.clearTiles();

    if (!Config::disableRoundTile() && !Config::disableOutlineTile()) {
        return;
    }

    for (const auto& screen: KWin::effects->screens()) {        // Per every screen
        const auto screen_region = getRegionWithoutMenus(screen->geometry());
        const auto geometry = screen_region.boundingRect();
        tileChecker.checkTiles(geometry);
    }
}

QRegion ShapeCorners::Effect::getRegionWithoutMenus(const QRect& screen_geometry)
{
    auto screen_region = QRegion(screen_geometry);
#ifdef DEBUG_MAXIMIZED
    qDebug() << "ShapeCorners: screen region" << screen_region;
#endif

    // subtract all menus
    for (const auto &ptr: m_menuBars) {
#ifdef DEBUG_MAXIMIZED
        qDebug() << "ShapeCorners: menu is" << ptr->frameGeometry();
#endif
        screen_region -= ptr->frameGeometry().toRect();
    }

#ifdef DEBUG_MAXIMIZED
    qDebug() << "ShapeCorners: screen region without menus" << screen_region;
#endif

    return screen_region;
}

void ShapeCorners::Effect::checkMaximized(KWin::EffectWindow *w) {
    auto window_iterator = m_managed.find(w);
    if (window_iterator == m_managed.end())
        return;

    window_iterator->second->isMaximized = false;

    auto screen_region = getRegionWithoutMenus(w->screen()->geometry());

    // check if window and screen match
    auto remaining = screen_region - w->frameGeometry().toRect();
#ifdef DEBUG_MAXIMIZED
    qDebug() << "ShapeCorners: active window remaining region" << remaining;
#endif
    if (remaining.isEmpty()) {
        window_iterator->second->isMaximized = true;
#ifdef DEBUG_MAXIMIZED
        qInfo() << "ShapeCorners: window maximized" << window_iterator->first->windowClass();
#endif
    }
}

void ShapeCorners::Effect::windowResized(KWin::EffectWindow *window, const QRectF &)
{
    checkTiled();
    checkMaximized(window);
}
