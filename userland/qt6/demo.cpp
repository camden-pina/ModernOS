// SPDX-License-Identifier: MIT
#include <QtWidgets>
#include <QtCore/QSaveFile>
#include <unistd.h>
Q_IMPORT_PLUGIN(CoastIntegrationPlugin)
int main(int argc,char **argv) {
 qputenv("QT_QPA_PLATFORM","coast");qputenv("QT_QPA_FONTDIR","/usr/share/fonts/coast");
 if(!qEnvironmentVariableIsSet("QT_SCALE_FACTOR"))qputenv("QT_SCALE_FACTOR","2");
 QApplication app(argc,argv);app.setApplicationName("Qt 6 Demo");app.setStyle("Fusion");app.setFont(QFont("DejaVu Sans",10));
 QWidget window;window.setWindowTitle("Qt 6 Demo");window.resize(820,530);
 auto *layout=new QVBoxLayout(&window);layout->setContentsMargins(26,22,26,22);layout->setSpacing(14);
 auto *title=new QLabel("Qt 6 on Coast");QFont heading=title->font();heading.setPointSize(22);heading.setBold(true);title->setFont(heading);layout->addWidget(title);
 auto *subtitle=new QLabel(QString("Core · Gui · Widgets     •     Native raster window     •     User %1").arg(getuid()));subtitle->setStyleSheet("color: #64748b;");layout->addWidget(subtitle);
 auto *tabs=new QTabWidget;layout->addWidget(tabs,1);
 auto *page=new QWidget;auto *form=new QVBoxLayout(page);form->setContentsMargins(18,18,18,18);form->setSpacing(12);
 auto *label=new QLabel("A real Qt layout: drag an edge, maximize, or snap this window.");form->addWidget(label);
 auto *edit=new QLineEdit;edit->setObjectName("name");edit->setPlaceholderText("Type something here…");edit->setContextMenuPolicy(Qt::NoContextMenu);form->addWidget(edit);
 auto *row=new QHBoxLayout;auto *button=new QPushButton("Add to list");button->setCursor(Qt::PointingHandCursor);row->addWidget(button);auto *check=new QCheckBox("Enable progress animation");row->addWidget(check);row->addStretch();form->addLayout(row);
 auto *list=new QListWidget;list->addItems({"Native keyboard and pointer input","Sharp text while resizing","Runs with ordinary account privileges"});form->addWidget(list,1);
 auto *slider=new QSlider(Qt::Horizontal);slider->setRange(0,100);slider->setValue(35);form->addWidget(slider);
 auto *progress=new QProgressBar;progress->setRange(0,100);progress->setValue(35);form->addWidget(progress);
 QObject::connect(slider,&QSlider::valueChanged,progress,&QProgressBar::setValue);
 auto add=[=]{if(!edit->text().isEmpty()){list->addItem(edit->text());qInfo("QT6_ADD text=%s count=%d",qPrintable(edit->text()),list->count());edit->clear();}};
 QObject::connect(button,&QPushButton::clicked,add);QObject::connect(edit,&QLineEdit::returnPressed,add);
 QTimer animation;animation.setInterval(45);QObject::connect(&animation,&QTimer::timeout,[=]{slider->setValue((slider->value()+1)%101);});QObject::connect(check,&QCheckBox::toggled,[&](bool on){if(on)animation.start();else animation.stop();});
 tabs->addTab(page,"Widgets");
 auto *document=new QWidget;auto *documentLayout=new QVBoxLayout(document);auto *text=new QPlainTextEdit;text->setObjectName("document");text->setContextMenuPolicy(Qt::NoContextMenu);text->setPlainText("Qt 6 uses Coast's own window service.\n\nThis editor supports selection, Unicode text, undo and redo.\nSave writes a real file in your profile's Documents folder.\n");documentLayout->addWidget(text);
 auto *saveRow=new QHBoxLayout;auto *save=new QPushButton("&Save document");auto *status=new QLabel;saveRow->addWidget(save);saveRow->addWidget(status,1);documentLayout->addLayout(saveRow);
 QObject::connect(save,&QPushButton::clicked,[=]{QString path=QDir::homePath()+"/Documents/Qt6-notes.txt";QSaveFile f(path);bool ok=f.open(QIODevice::WriteOnly);QByteArray bytes=text->toPlainText().toUtf8();ok=ok && f.write(bytes)==bytes.size() && f.commit();status->setText(ok?"Saved to Documents/Qt6-notes.txt":"Save failed: "+f.errorString());qInfo("QT6_SAVE success=%d path=%s",ok,qPrintable(path));});
 tabs->addTab(document,"Text editor");
 auto *footer=new QLabel(QString("Software rendering · Qt 6.8.3 · %1").arg(geteuid()?"Standard account privileges":"Running as root"));footer->setStyleSheet("color: #64748b;");layout->addWidget(footer);
 window.show();edit->setFocus();qInfo("QT6_READY version=%s uid=%u",qVersion(),unsigned(getuid()));return app.exec();
}
