// -*- mode: c++ -*-
#pragma once

#include <QtWidgets/QMainWindow>

#include "MyViewer.h"

class QApplication;
class QProgressBar;

class MyWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MyWindow(QApplication *parent);
    ~MyWindow();

private slots:
    void open();
    void save();
    void loadfav();
    void setCutoff();
    void setRange();
    void setSlicing();
    void setGrid();
    void setAngleLimit();
    void setFavoriteModel();
    void toggleCones();
    void calcTreePoints();
    void startComputation(QString message);
    void midComputation(int percent);
    void endComputation();

private:
    QApplication *parent;
    MyViewer *viewer;
    QProgressBar *progress;
    QString last_directory;
    QString favPath = "C:\\clever-support\\build\\gula.stl";
};
