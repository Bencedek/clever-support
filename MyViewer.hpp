#pragma once
#include "MyViewer.h"

double MyViewer::getCutoffRatio() const {
    return cutoff_ratio;
}

void MyViewer::setCutoffRatio(double ratio) {
    cutoff_ratio = ratio;
    updateMeanMinMax();
}

double MyViewer::getMeanMin() const {
    return mean_min;
}

void MyViewer::setMeanMin(double min) {
    mean_min = min;
}

double MyViewer::getMeanMax() const {
    return mean_max;
}

void MyViewer::setMeanMax(double max) {
    mean_max = max;
}

double MyViewer::getGridDensity() const {
    return gridDensity;
}

void MyViewer::setGridDensity(double d) {
    gridDensity = d;
}

double MyViewer::getAngleLimit() const {
    return angleLimit;
}

void MyViewer::setAngleLimit(double a) {
    angleLimit = a;
}

void MyViewer::toggleCones() {
    showCones = !showCones;
}

void MyViewer::toggleTree() {
    showTree = !showTree;
}

const double *MyViewer::getSlicingDir() const {
    return slicing_dir.data();
}

void MyViewer::setSlicingDir(double x, double y, double z) {
    slicing_dir = Vector(x, y, z).normalized();
}

double MyViewer::getSlicingScaling() const {
    return slicing_scaling;
}

void MyViewer::setSlicingScaling(double scaling) {
    slicing_scaling = scaling;
}

