// SPDX-License-Identifier: MIT
// Qt 6.8.3 QPA: copied raster frames and owned input queues, without a GPU,
// X server, Wayland server, display mapping or elevated client credentials.
#include <QtCore/QTimer>
#include <QtCore/QDebug>
#include <QtCore/QMimeData>
#include <QtGui/QGuiApplication>
#include <QtGui/QWindow>
#include <QtGui/QCursor>
#include <QtGui/QImage>
#include <QtGui/QScreen>
#include <QtGui/private/qgenericunixeventdispatcher_p.h>
#include <QtGui/private/qfreetypefontdatabase_p.h>
#include <qpa/qplatformintegrationplugin.h>
#include <qpa/qplatformintegration.h>
#include <qpa/qplatformscreen.h>
#include <qpa/qplatformwindow.h>
#include <qpa/qplatformbackingstore.h>
#include <qpa/qplatformcursor.h>
#include <qpa/qplatformclipboard.h>
#include <qpa/qwindowsysteminterface.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <cerrno>
#include <cstring>
#include <uapi/window.h>
#include <uapi/clipboard.h>
#include <memory>

static long native(unsigned op,void *data,size_t size) {long r=::syscall(OS_WINDOW_SYSCALL,op,data,size);return r<0?-errno:r;}
static Qt::KeyboardModifiers modifiers(unsigned m) {Qt::KeyboardModifiers r;if(m&1)r|=Qt::ControlModifier;if(m&2)r|=Qt::ShiftModifier;if(m&4)r|=Qt::AltModifier;if(m&8)r|=Qt::MetaModifier;return r;}
static int key(unsigned hid,unsigned ascii) {
 if(hid>=4 && hid<=29)return Qt::Key_A+int(hid-4);
 if(hid>=30 && hid<=39)return "1234567890"[hid-30];
 if(hid>=58 && hid<=69)return Qt::Key_F1+int(hid-58);
 switch(hid) {
 case 40:return Qt::Key_Return;case 41:return Qt::Key_Escape;case 42:return Qt::Key_Backspace;case 43:return Qt::Key_Tab;
 case 44:return Qt::Key_Space;case 45:return Qt::Key_Minus;case 46:return Qt::Key_Equal;case 47:return Qt::Key_BracketLeft;case 48:return Qt::Key_BracketRight;
 case 49:return Qt::Key_Backslash;case 51:return Qt::Key_Semicolon;case 52:return Qt::Key_Apostrophe;case 53:return Qt::Key_QuoteLeft;case 54:return Qt::Key_Comma;case 55:return Qt::Key_Period;case 56:return Qt::Key_Slash;
 case 57:return Qt::Key_CapsLock;case 73:return Qt::Key_Insert;case 74:return Qt::Key_Home;case 75:return Qt::Key_PageUp;case 76:return Qt::Key_Delete;case 77:return Qt::Key_End;case 78:return Qt::Key_PageDown;
 case 79:return Qt::Key_Right;case 80:return Qt::Key_Left;case 81:return Qt::Key_Down;case 82:return Qt::Key_Up;
 case 224:case 228:return Qt::Key_Control;case 225:case 229:return Qt::Key_Shift;case 226:case 230:return Qt::Key_Alt;case 227:case 231:return Qt::Key_Meta;
 default:return ascii>='a'&&ascii<='z'?int(ascii-32):int(ascii);
 }
}
class ModernWindow:public QPlatformWindow {
 QTimer timer;
 Qt::MouseButtons buttons;
 Qt::KeyboardModifiers mods;
 bool inside=false,held[256]={};
 unsigned id=0;
public:
 explicit ModernWindow(QWindow *w):QPlatformWindow(w) {
  QRect r=geometry();r.setSize(r.size().expandedTo(QSize(160,100)).boundedTo(QSize(OS_WINDOW_PIXEL_MAX_WIDTH,OS_WINDOW_PIXEL_MAX_HEIGHT)));QPlatformWindow::setGeometry(r);
  os_window_info info={};info.version=OS_WINDOW_PIXEL_CLIENT_VERSION;info.width=r.width();info.height=r.height();
  QByteArray title=w->title().toUtf8();if(title.isEmpty())title="Qt 6 Demo";std::memcpy(info.title,title.constData(),qMin(title.size(),qsizetype(63)));
  long e=native(OS_WINDOW_CREATE,&info,sizeof(info));if(e)qFatal("Coast Qt window creation failed: %ld",e);id=info.id;
  QObject::connect(&timer,&QTimer::timeout,[this]{poll();});timer.start(8);
  qInfo("QT6_WINDOW id=%u uid=%u width=%d height=%d",id,unsigned(getuid()),r.width(),r.height());
 }
 ~ModernWindow() override {timer.stop();os_window_token t={id,0};native(OS_WINDOW_CLIENT_CLOSE,&t,sizeof(t));}
 WId winId() const override {return id;}
 void requestActivateWindow() override {} // The desktop supplies focus events.
 void propagateSizeHints() override {} // The compositor constrains the client.
 void setVisible(bool visible) override {QWindowSystemInterface::handleExposeEvent(window(),visible?QRegion(QRect(QPoint(),geometry().size())):QRegion());}
 void setGeometry(const QRect &r) override {QPlatformWindow::setGeometry(r);}
 void setWindowState(Qt::WindowStates state) override {
  unsigned action=state&Qt::WindowFullScreen?OS_WINDOW_REQUEST_FULLSCREEN:state&Qt::WindowMaximized?OS_WINDOW_REQUEST_MAXIMIZE:state&Qt::WindowMinimized?OS_WINDOW_REQUEST_MINIMIZE:OS_WINDOW_REQUEST_RESTORE;
  os_window_request request={id,action};native(OS_WINDOW_REQUEST_STATE,&request,sizeof(request));
 }
 void poll() {
  os_window_event events[32];os_window_events request={id,32,reinterpret_cast<uintptr_t>(events)};
  long n=native(OS_WINDOW_INPUT,&request,sizeof(request));
  if(n==-EPIPE){QWindowSystemInterface::handleCloseEvent(window());timer.stop();return;}
  for(long i=0;i<n;i++) {
   const auto &e=events[i];
   if(e.type==OS_WINDOW_CLOSE){QWindowSystemInterface::handleCloseEvent(window());timer.stop();}
   else if(e.type==OS_WINDOW_CONFIGURE) {
    QRect r=geometry();r.setSize(QSize(e.dx,e.dy));QPlatformWindow::setGeometry(r);
    Qt::WindowStates state;if(e.code&OS_WINDOW_STATE_MAXIMIZED)state|=Qt::WindowMaximized;if(e.code&OS_WINDOW_STATE_MINIMIZED)state|=Qt::WindowMinimized;if(e.code&OS_WINDOW_STATE_FULLSCREEN)state|=Qt::WindowFullScreen;
    QWindowSystemInterface::handleWindowStateChanged(window(),state);QWindowSystemInterface::handleGeometryChange(window(),r);QWindowSystemInterface::handleExposeEvent(window(),QRegion(QRect(QPoint(),r.size())));
    qInfo("QT6_CONFIGURE width=%d height=%d states=%u",e.dx,e.dy,e.code);
   } else if(e.type==OS_WINDOW_FOCUS) {
    if(!e.code){mods={};buttons={};std::memset(held,0,sizeof(held));}
    QWindowSystemInterface::handleFocusWindowChanged(e.code?window():nullptr);
   } else if(e.type==OS_WINDOW_OVERFLOW) {mods={};buttons={};std::memset(held,0,sizeof(held));}
   else if(e.type==OS_WINDOW_POINTER) {
    QPointF p(e.dx,e.dy),global=p+geometry().topLeft();
    if(bool(e.code)!=inside){inside=e.code;if(inside)QWindowSystemInterface::handleEnterEvent(window(),p,global);else QWindowSystemInterface::handleLeaveEvent(window());}
    Qt::MouseButtons next;if(e.buttons&1)next|=Qt::LeftButton;if(e.buttons&2)next|=Qt::RightButton;if(e.buttons&4)next|=Qt::MiddleButton;
    for(auto button:{Qt::LeftButton,Qt::RightButton,Qt::MiddleButton})if(bool(next&button)!=bool(buttons&button)) {
     bool down=next&button;buttons.setFlag(button,down);QWindowSystemInterface::handleMouseEvent(window(),p,global,buttons,button,down?QEvent::MouseButtonPress:QEvent::MouseButtonRelease,mods);
    }
    if(inside)QWindowSystemInterface::handleMouseEvent(window(),p,global,buttons,Qt::NoButton,QEvent::MouseMove,mods);
    if(e.wheel)QWindowSystemInterface::handleWheelEvent(window(),p,global,QPoint(),QPoint(0,e.wheel*120),mods);
   } else if(e.type==OS_WINDOW_HID_KEY || e.type==OS_WINDOW_KEY) {
    bool physical=e.type==OS_WINDOW_HID_KEY,down=!physical || e.buttons;unsigned ascii=physical?e.dx:e.code;
    if(physical)mods=modifiers(e.dy);
    char32_t character=ascii;QString text=ascii>=32 && ascii!=127?QString::fromUcs4(&character,1):QString();int code=key(physical?e.code:0,ascii);
    if(!physical){if(ascii==10 || ascii==13)code=Qt::Key_Return;else if(ascii==9)code=Qt::Key_Tab;else if(ascii==8 || ascii==127)code=Qt::Key_Backspace;}
    bool repeat=physical && e.code<256 && held[e.code] && down;
    QWindowSystemInterface::handleKeyEvent(window(),down?QEvent::KeyPress:QEvent::KeyRelease,code,mods,text,repeat);
    if(physical && e.code<256)held[e.code]=down;
    if(!physical)QWindowSystemInterface::handleKeyEvent(window(),QEvent::KeyRelease,code,mods,text);
   }
  }
 }
};
class ModernBackingStore:public QPlatformBackingStore {
 QImage image;
 bool first=true;
public:
 explicit ModernBackingStore(QWindow *w):QPlatformBackingStore(w){}
 QPaintDevice *paintDevice() override {return &image;}
 void resize(const QSize &size,const QRegion &) override {if(size!=image.size()){image=QImage(size,QImage::Format_ARGB32_Premultiplied);image.fill(0xfff6f7f9);first=true;}}
 void flush(QWindow *w,const QRegion &region,const QPoint &offset) override {
  if(image.isNull() || !w->handle())return;
  QRect r=first?image.rect():region.boundingRect().translated(offset).intersected(image.rect());if(r.isEmpty())return;
  os_window_damage_frame f={};f.frame.id=w->handle()->winId();f.frame.width=image.width();f.frame.height=image.height();f.frame.stride=image.bytesPerLine()/4;f.frame.pixels=reinterpret_cast<uintptr_t>(image.constBits());f.x=r.x();f.y=r.y();f.width=r.width();f.height=r.height();
  long result=native(OS_WINDOW_PRESENT_DAMAGE,&f,sizeof(f));
  if(!result && first){first=false;qInfo("QT6_PRESENT width=%d height=%d dpr=%.1f",image.width(),image.height(),w->devicePixelRatio());}
  else if(result && result!=-EPIPE)qWarning("QT6_PRESENT_ERROR result=%ld width=%d height=%d",result,image.width(),image.height());
 }
};
class ModernCursor:public QPlatformCursor {
public:
 void changeCursor(QCursor *cursor,QWindow *w) override {
  if(!w || !w->handle())return;os_window_cursor c={};c.id=w->handle()->winId();
  QImage pixels;
  if(cursor && cursor->shape()==Qt::BlankCursor)c.mode=OS_WINDOW_CURSOR_HIDDEN;
  else if(cursor && cursor->shape()!=Qt::ArrowCursor) {
   QPoint hot=cursor->hotSpot();
   if(cursor->shape()==Qt::BitmapCursor)pixels=cursor->pixmap().toImage();
   else {QPlatformCursorImage generated(nullptr,nullptr,0,0,0,0);generated.set(cursor->shape());pixels=*generated.image();hot=generated.hotspot();}
   qreal scale=qBound(0.5,w->devicePixelRatio(),4.0);
   if(!pixels.isNull()){pixels=pixels.scaled(QSize(qRound(pixels.width()*scale),qRound(pixels.height()*scale)),Qt::IgnoreAspectRatio,Qt::SmoothTransformation).convertToFormat(QImage::Format_ARGB32_Premultiplied);c.mode=OS_WINDOW_CURSOR_BITMAP;c.width=pixels.width();c.height=pixels.height();c.stride=pixels.bytesPerLine()/4;c.hotspot_x=qRound(hot.x()*scale);c.hotspot_y=qRound(hot.y()*scale);c.pixels=reinterpret_cast<uintptr_t>(pixels.constBits());}
  }
  native(OS_WINDOW_CURSOR,&c,sizeof(c));
 }
};
class ModernScreen:public QPlatformScreen {
 mutable ModernCursor pointer;
public:
 QRect geometry() const override {return QRect(0,0,1920,1080);}
 int depth() const override {return 32;}
 QImage::Format format() const override {return QImage::Format_ARGB32_Premultiplied;}
 QDpi logicalDpi() const override {return QDpi(96,96);}
 QPlatformCursor *cursor() const override {return &pointer;}
};
class ModernClipboard:public QPlatformClipboard {
 QTimer timer;
 std::unique_ptr<QMimeData> data=std::make_unique<QMimeData>();
 QMimeData empty;
 uint64_t generation=0,loaded=0,owned=0;
 static long call(unsigned op,void *p,size_t bytes,uint64_t expected=0) {
  long r=::syscall(COAST_SYSCALL_CLIPBOARD,op,p,bytes,expected);return r<0?-errno:r;
 }
 static bool info(os_clipboard_info &i) {
  return !call(OS_CLIPBOARD_INFO,&i,sizeof(i)) && i.capacity==OS_CLIPBOARD_LIMIT && i.bytes<=i.capacity;
 }
public:
 ModernClipboard(){QObject::connect(&timer,&QTimer::timeout,[this]{os_clipboard_info i={};if(info(i) && i.generation!=generation){generation=i.generation;owned=0;emitChanged(QClipboard::Clipboard);}});timer.start(150);}
 bool supportsMode(QClipboard::Mode mode) const override {return mode==QClipboard::Clipboard;}
 bool ownsMode(QClipboard::Mode mode) const override {os_clipboard_info i={};return supportsMode(mode) && owned && info(i) && i.generation==owned;}
 QMimeData *mimeData(QClipboard::Mode mode) override {
  if(!supportsMode(mode))return &empty;
  for(unsigned retry=0;retry<4;retry++) {
   os_clipboard_info i={};if(!info(i))return &empty;
   if(loaded==i.generation)return data.get();
   QByteArray bytes(int(i.bytes),Qt::Uninitialized);
   long result=call(OS_CLIPBOARD_READ,bytes.data(),bytes.size(),i.generation);
   if(result==-EAGAIN)continue;
   if(result!=bytes.size())break;
   generation=loaded=i.generation;data=std::make_unique<QMimeData>();data->setText(QString::fromUtf8(bytes));return data.get();
  }
  return &empty;
 }
 void setMimeData(QMimeData *value,QClipboard::Mode mode) override {
  if(value==data.get() || value==&empty)return;
  std::unique_ptr<QMimeData> incoming(value);
  if(!supportsMode(mode) || (value && !value->hasText()))return;
  QByteArray bytes=value?value->text().toUtf8():QByteArray();
  if(bytes.size()>OS_CLIPBOARD_LIMIT){qWarning("Qt clipboard text exceeds 64 KiB");return;}
  long result=call(OS_CLIPBOARD_WRITE,bytes.data(),bytes.size());
  if(result!=bytes.size()){qWarning("Qt clipboard write denied: %ld",result);return;}
  os_clipboard_info i={};owned=info(i)?i.generation:0;generation=loaded=owned;
  data=std::move(incoming);if(!data)data=std::make_unique<QMimeData>();
  emitChanged(QClipboard::Clipboard);
 }
};
class ModernIntegration:public QPlatformIntegration {
 ModernScreen *screen=new ModernScreen;
 mutable QFreeTypeFontDatabase fonts;
 mutable std::unique_ptr<ModernClipboard> clip;
public:
 ModernIntegration(){QWindowSystemInterface::handleScreenAdded(screen);}
 ~ModernIntegration() override {QWindowSystemInterface::handleScreenRemoved(screen);}
 bool hasCapability(Capability c) const override {if(c==RhiBasedRendering || c==OpenGL || c==MultipleWindows || c==NativeWidgets || c==ScreenWindowGrabbing)return false;return QPlatformIntegration::hasCapability(c);}
 QPlatformWindow *createPlatformWindow(QWindow *w) const override {return new ModernWindow(w);}
 QPlatformBackingStore *createPlatformBackingStore(QWindow *w) const override {return new ModernBackingStore(w);}
 QAbstractEventDispatcher *createEventDispatcher() const override {return createUnixEventDispatcher();}
 QPlatformFontDatabase *fontDatabase() const override {return &fonts;}
 QPlatformClipboard *clipboard() const override {if(!clip)clip=std::make_unique<ModernClipboard>();return clip.get();}
};
class CoastIntegrationPlugin:public QPlatformIntegrationPlugin {
 Q_OBJECT
 Q_PLUGIN_METADATA(IID QPlatformIntegrationFactoryInterface_iid FILE "coast.json")
public:
 QPlatformIntegration *create(const QString &name,const QStringList &) override {return name==QLatin1String("coast")?new ModernIntegration:nullptr;}
};
#include "coast.moc"
