#include <memory>

#include <QtWidgets>

#include "MyWindow.h"

MyWindow::MyWindow(QApplication *parent) :
    QMainWindow(), parent(parent), last_directory(".")
{
    setWindowTitle(tr("Sample 3D Framework"));
    setStatusBar(new QStatusBar);
    progress = new QProgressBar;
    progress->setMinimum(0); progress->setMaximum(100);
    progress->hide();
    statusBar()->addPermanentWidget(progress);

    viewer = new MyViewer(this);
    connect(viewer, SIGNAL(startComputation(QString)), this, SLOT(startComputation(QString)));
    connect(viewer, SIGNAL(midComputation(int)), this, SLOT(midComputation(int)));
    connect(viewer, SIGNAL(endComputation()), this, SLOT(endComputation()));
    setCentralWidget(viewer);

    /////////////////////////
    // Setup actions/menus //
    /////////////////////////

    auto openAction = new QAction(tr("&Open"), this);
    openAction->setShortcut(tr("Ctrl+O"));
    openAction->setStatusTip(tr("Load a model from a file"));
    connect(openAction, SIGNAL(triggered()), this, SLOT(open()));

    auto saveAction = new QAction(tr("&Save as.."), this);
    saveAction->setShortcut(tr("Ctrl+Alt+S"));
    saveAction->setStatusTip(tr("Save to a file"));
    connect(saveAction, SIGNAL(triggered()), this, SLOT(save()));

    auto quitAction = new QAction(tr("&Quit"), this);
    quitAction->setShortcut(tr("Ctrl+Q"));
    quitAction->setStatusTip(tr("Quit the program"));
    connect(quitAction, SIGNAL(triggered()), this, SLOT(close()));

    auto loadfavAction = new QAction(tr("&Load favorite"), this);
    loadfavAction->setShortcut(tr("Ctrl+X"));
    connect(loadfavAction, SIGNAL(triggered()), this, SLOT(loadfav()));

    auto cutoffAction = new QAction(tr("Set &cutoff ratio"), this);
    cutoffAction->setStatusTip(tr("Set mean map cutoff ratio"));
    connect(cutoffAction, SIGNAL(triggered()), this, SLOT(setCutoff()));

    auto rangeAction = new QAction(tr("Set &range"), this);
    rangeAction->setStatusTip(tr("Set mean map range"));
    connect(rangeAction, SIGNAL(triggered()), this, SLOT(setRange()));

    auto slicingAction = new QAction(tr("Set &slicing parameters"), this);
    rangeAction->setStatusTip(tr("Set contouring direction and scaling"));
    connect(slicingAction, SIGNAL(triggered()), this, SLOT(setSlicing()));

    auto angleLimitAction = new QAction(tr("Set overhang &angle limit"), this);
    connect(angleLimitAction, SIGNAL(triggered()), this, SLOT(setAngleLimit()));

    auto gridAction = new QAction(tr("Set support &grid density"), this);
    connect(gridAction, SIGNAL(triggered()), this, SLOT(setGrid()));

    auto coefficientAction = new QAction(tr("Set &diameter coefficient"), this);
    connect(coefficientAction, SIGNAL(triggered()), this, SLOT(setDiameterCoefficient()));

    auto toggleConesAction = new QAction(tr("Toggle cones"), this);
    connect(toggleConesAction, SIGNAL(triggered()), this, SLOT(toggleCones()));

    auto calculateTreeAction = new QAction(tr("Refresh support tree points"), this);
    connect(calculateTreeAction, SIGNAL(triggered()), this, SLOT(calculateTreePoints()));

    auto treePointsAction = new QAction(tr("Toggle support tree"), this);
    connect(treePointsAction, SIGNAL(triggered()), this, SLOT(toggleTree()));

    auto addTreeGeometryAction = new QAction(tr("Add support tree geometry"), this);
    connect(addTreeGeometryAction, SIGNAL(triggered()), this, SLOT(addTreeGeometry()));

    auto setFavoriteAction = new QAction(tr("Set favorite model"), this);
    connect(setFavoriteAction, SIGNAL(triggered()), this, SLOT(setFavoriteModel()));

    auto fileMenu = menuBar()->addMenu(tr("&File"));
    fileMenu->addAction(openAction);
    fileMenu->addAction(saveAction);
    fileMenu->addAction(loadfavAction);
    fileMenu->addAction(quitAction);

    auto visMenu = menuBar()->addMenu(tr("&Visualization"));
    visMenu->addAction(cutoffAction);
    visMenu->addAction(rangeAction);
    visMenu->addAction(slicingAction);

    auto supportMenu = menuBar()->addMenu(tr("&Support settings"));
    supportMenu->addAction(angleLimitAction);
    supportMenu->addAction(gridAction);
    supportMenu->addAction(coefficientAction);
    supportMenu->addAction(toggleConesAction);
    supportMenu->addAction(calculateTreeAction);
    supportMenu->addAction(treePointsAction);
    supportMenu->addAction(addTreeGeometryAction);
    supportMenu->addAction(setFavoriteAction);

}

MyWindow::~MyWindow() {
}

void MyWindow::open() {
    auto filename =
        QFileDialog::getOpenFileName(this, tr("Open File"), last_directory,
                                     tr("Readable files (*.obj *.ply *.stl *.bzr);;"
                                        "Mesh (*.obj *.ply *.stl);;"
                                        "Bézier surface (*.bzr);;"
                                        "All files (*.*)"));
    if(filename.isEmpty())
        return;
    last_directory = QFileInfo(filename).absolutePath();

    bool ok;
    if (filename.endsWith(".bzr"))
        ok = viewer->openBezier(filename.toUtf8().data());
    else
        ok = viewer->openMesh(filename.toUtf8().data());

    if (!ok)
        QMessageBox::warning(this, tr("Cannot open file"),
                             tr("Could not open file: ") + filename + ".");
}

void MyWindow::save() {
    auto filename =
        QFileDialog::getSaveFileName(this, tr("Save File"), last_directory,
                                     tr("Bézier surface (*.bzr);;STL file (*.stl)"));
        if(filename.isEmpty())
        return;
    last_directory = QFileInfo(filename).absolutePath();

    if (!viewer->saveMesh(filename.toUtf8().data()))
        QMessageBox::warning(this, tr("Cannot save file"),
                             tr("Could not save file: ") + filename + ".");
}

void MyWindow::loadfav() {
    bool ok = viewer->openMesh(favPath.toUtf8().data());

    if (!ok)
        QMessageBox::warning(this, tr("Cannot open file"),
                             tr("Could not open file"));
}

void MyWindow::setCutoff() {
    // Memory management options for the dialog:
    // - on the stack (deleted at the end of the function)
    // - on the heap with manual delete or std::unique_ptr
    // There is also a Qt-controlled automatic deletion by calling
    //     dlg->setAttribute(Qt::WA_DeleteOnClose);
    // ... but this could delete sub-widgets too early.

    auto dlg = std::make_unique<QDialog>(this);
    auto *hb1    = new QHBoxLayout,
        *hb2    = new QHBoxLayout;
    auto *vb     = new QVBoxLayout;
    auto *text   = new QLabel(tr("Cutoff ratio:"));
    auto *sb     = new QDoubleSpinBox;
    auto *cancel = new QPushButton(tr("Cancel"));
    auto *ok     = new QPushButton(tr("Ok"));

    sb->setDecimals(3);
    sb->setRange(0.001, 0.5);
    sb->setSingleStep(0.01);
    sb->setValue(viewer->getCutoffRatio());
    connect(cancel, SIGNAL(pressed()), dlg.get(), SLOT(reject()));
    connect(ok,     SIGNAL(pressed()), dlg.get(), SLOT(accept()));
    ok->setDefault(true);

    hb1->addWidget(text);
    hb1->addWidget(sb);
    hb2->addWidget(cancel);
    hb2->addWidget(ok);
    vb->addLayout(hb1);
    vb->addLayout(hb2);

    dlg->setWindowTitle(tr("Set ratio"));
    dlg->setLayout(vb);

    if(dlg->exec() == QDialog::Accepted) {
        viewer->setCutoffRatio(sb->value());
        viewer->update();
    }
}

void MyWindow::setRange() {
    QDialog dlg(this);
    auto *grid   = new QGridLayout;
    auto *text1  = new QLabel(tr("Min:")),
        *text2  = new QLabel(tr("Max:"));
    auto *sb1    = new QDoubleSpinBox,
        *sb2    = new QDoubleSpinBox;
    auto *cancel = new QPushButton(tr("Cancel"));
    auto *ok     = new QPushButton(tr("Ok"));

    // The range of the spinbox controls the number of displayable digits,
    // so setting it to a large value results in a very wide window.
    double max = 1000.0; // std::numeric_limits<double>::max();
    sb1->setDecimals(5);                 sb2->setDecimals(5);
    sb1->setRange(-max, 0.0);            sb2->setRange(0.0, max);
    sb1->setSingleStep(0.01);            sb2->setSingleStep(0.01);
    sb1->setValue(viewer->getMeanMin()); sb2->setValue(viewer->getMeanMax());
    connect(cancel, SIGNAL(pressed()), &dlg, SLOT(reject()));
    connect(ok,     SIGNAL(pressed()), &dlg, SLOT(accept()));
    ok->setDefault(true);

    grid->addWidget( text1, 1, 1, Qt::AlignRight);
    grid->addWidget(   sb1, 1, 2);
    grid->addWidget( text2, 2, 1, Qt::AlignRight);
    grid->addWidget(   sb2, 2, 2);
    grid->addWidget(cancel, 3, 1);
    grid->addWidget(    ok, 3, 2);

    dlg.setWindowTitle(tr("Set range"));
    dlg.setLayout(grid);

    if(dlg.exec() == QDialog::Accepted) {
        viewer->setMeanMin(sb1->value());
        viewer->setMeanMax(sb2->value());
        viewer->update();
    }
}

void MyWindow::setSlicing() {
    auto dlg = std::make_unique<QDialog>(this);
    auto *hb1    = new QHBoxLayout,
        *hb2    = new QHBoxLayout,
            *hb3    = new QHBoxLayout;
    auto *vb     = new QVBoxLayout;
    auto *text_v = new QLabel(tr("Slicing direction:"));
    QDoubleSpinBox *sb_v[3];
    auto *text_s = new QLabel(tr("Slicing scaling:"));
    auto *sb_s   = new QDoubleSpinBox;
    auto *cancel = new QPushButton(tr("Cancel"));
    auto *ok     = new QPushButton(tr("Ok"));

    for (int i = 0; i < 3; ++i) {
        sb_v[i] = new QDoubleSpinBox;
        sb_v[i]->setDecimals(3);
        sb_v[i]->setRange(-1, 1);
        sb_v[i]->setSingleStep(0.01);
        sb_v[i]->setValue(viewer->getSlicingDir()[i]);
    }
    sb_s->setDecimals(6);
    sb_s->setRange(0, 10000);
    sb_s->setSingleStep(1);
    sb_s->setValue(viewer->getSlicingScaling());
    connect(cancel, SIGNAL(pressed()), dlg.get(), SLOT(reject()));
    connect(ok,     SIGNAL(pressed()), dlg.get(), SLOT(accept()));
    ok->setDefault(true);

    hb1->addWidget(text_v);
    hb1->addWidget(sb_v[0]); hb1->addWidget(sb_v[1]); hb1->addWidget(sb_v[2]);
    hb2->addWidget(text_s);
    hb2->addWidget(sb_s);
    hb3->addWidget(cancel);
    hb3->addWidget(ok);
    vb->addLayout(hb1);
    vb->addLayout(hb2);
    vb->addLayout(hb3);

    dlg->setWindowTitle(tr("Set slicing"));
    dlg->setLayout(vb);

    if(dlg->exec() == QDialog::Accepted) {
        viewer->setSlicingDir(sb_v[0]->value(), sb_v[1]->value(), sb_v[2]->value());
        viewer->setSlicingScaling(sb_s->value());
        viewer->update();
    }
}

void MyWindow::setAngleLimit() {
    auto dlg = std::make_unique<QDialog>(this);
    auto *hb1    = new QHBoxLayout,
        *hb2    = new QHBoxLayout;
    auto *vb     = new QVBoxLayout;
    auto *text   = new QLabel(tr("Angle overhang limit (deg):"));
    auto *sb     = new QDoubleSpinBox;
    auto *cancel = new QPushButton(tr("Cancel"));
    auto *ok     = new QPushButton(tr("Ok"));

    sb->setDecimals(1);
    sb->setRange(0, 90);
    sb->setSingleStep(1);
    sb->setValue(viewer->getAngleLimit() * 180 / M_PI);
    connect(cancel, SIGNAL(pressed()), dlg.get(), SLOT(reject()));
    connect(ok,     SIGNAL(pressed()), dlg.get(), SLOT(accept()));
    ok->setDefault(true);

    hb1->addWidget(text);
    hb1->addWidget(sb);
    hb2->addWidget(cancel);
    hb2->addWidget(ok);
    vb->addLayout(hb1);
    vb->addLayout(hb2);

    dlg->setWindowTitle(tr("Set angle overhang limit"));
    dlg->setLayout(vb);

    if(dlg->exec() == QDialog::Accepted) {
        viewer->setAngleLimit(sb->value() * M_PI / 180);
        viewer->update();
    }
}

void MyWindow::setGrid() {
    auto dlg = std::make_unique<QDialog>(this);
    auto *hb1    = new QHBoxLayout,
        *hb2    = new QHBoxLayout;
    auto *vb     = new QVBoxLayout;
    auto *text   = new QLabel(tr("Grid density:"));
    auto *sb     = new QDoubleSpinBox;
    auto *cancel = new QPushButton(tr("Cancel"));
    auto *ok     = new QPushButton(tr("Ok"));

    sb->setDecimals(0);
    sb->setRange(2, 100);
    sb->setSingleStep(2);
    sb->setValue(viewer->getGridDensity());
    connect(cancel, SIGNAL(pressed()), dlg.get(), SLOT(reject()));
    connect(ok,     SIGNAL(pressed()), dlg.get(), SLOT(accept()));
    ok->setDefault(true);

    hb1->addWidget(text);
    hb1->addWidget(sb);
    hb2->addWidget(cancel);
    hb2->addWidget(ok);
    vb->addLayout(hb1);
    vb->addLayout(hb2);

    dlg->setWindowTitle(tr("Set support grid density"));
    dlg->setLayout(vb);

    if(dlg->exec() == QDialog::Accepted) {
        viewer->setGridDensity(sb->value());
        viewer->update();
    }
}

void MyWindow::setDiameterCoefficient() {
    auto dlg = std::make_unique<QDialog>(this);
    auto *hb1    = new QHBoxLayout,
        *hb2    = new QHBoxLayout;
    auto *vb     = new QVBoxLayout;
    auto *text   = new QLabel(tr("Diameter coefficient:"));
    auto *sb     = new QDoubleSpinBox;
    auto *cancel = new QPushButton(tr("Cancel"));
    auto *ok     = new QPushButton(tr("Ok"));

    sb->setDecimals(4);
    sb->setRange(0, 1);
    sb->setSingleStep(0.001);
    sb->setValue(viewer->getDiameterCoefficient());
    connect(cancel, SIGNAL(pressed()), dlg.get(), SLOT(reject()));
    connect(ok,     SIGNAL(pressed()), dlg.get(), SLOT(accept()));
    ok->setDefault(true);

    hb1->addWidget(text);
    hb1->addWidget(sb);
    hb2->addWidget(cancel);
    hb2->addWidget(ok);
    vb->addLayout(hb1);
    vb->addLayout(hb2);

    dlg->setWindowTitle(tr("Set diameter coefficient"));
    dlg->setLayout(vb);

    if(dlg->exec() == QDialog::Accepted) {
        viewer->setDiameterCoefficient(sb->value());
        viewer->update();
    }
}

void MyWindow::setFavoriteModel() {
    auto filename =
        QFileDialog::getOpenFileName(this, tr("Open File"), last_directory,
                                     tr("Readable files (*.obj *.ply *.stl *.bzr);;"
                                        "Mesh (*.obj *.ply *.stl);;"
                                        "Bézier surface (*.bzr);;"
                                        "All files (*.*)"));
    if(filename.isEmpty())
        return;
    favPath = QFileInfo(filename).filePath();
}

void MyWindow::toggleCones() {
    viewer->toggleCones();
    viewer->update();
}

void MyWindow::calculateTreePoints() {
    viewer->calculateSupportTreePoints();
    viewer->update();
}

void MyWindow::toggleTree() {
    viewer->toggleTree();
    viewer->update();
}

void MyWindow::addTreeGeometry(){
    viewer->addTreeGeometry();
    viewer->update();
}

void MyWindow::startComputation(QString message) {
    statusBar()->showMessage(message);
    progress->setValue(0);
    progress->show();
    parent->processEvents(QEventLoop::ExcludeUserInputEvents);
}

void MyWindow::midComputation(int percent) {
    progress->setValue(percent);
    parent->processEvents(QEventLoop::ExcludeUserInputEvents);
}

void MyWindow::endComputation() {
    progress->hide();
    statusBar()->clearMessage();
}
