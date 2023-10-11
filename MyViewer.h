// -*- mode: c++ -*-
#pragma once

#include <string>
#include <deque>

#include <QGLViewer/qglviewer.h>
#include <OpenMesh/Core/Mesh/TriMesh_ArrayKernelT.hh>

using qglviewer::Vec;

class MyViewer : public QGLViewer {
    Q_OBJECT

public:
    explicit MyViewer(QWidget *parent);
    virtual ~MyViewer();

    inline double getCutoffRatio() const;
    inline void setCutoffRatio(double ratio);
    inline double getMeanMin() const;
    inline void setMeanMin(double min);
    inline double getMeanMax() const;
    inline void setMeanMax(double max);
    inline const double *getSlicingDir() const;
    inline void setSlicingDir(double x, double y, double z);
    inline double getSlicingScaling() const;
    inline void setSlicingScaling(double scaling);
    bool openMesh(const std::string &filename, bool update_view = true);
    bool openBezier(const std::string &filename, bool update_view = true);
    bool saveBezier(const std::string &filename);

signals:
    void startComputation(QString message);
    void midComputation(int percent);
    void endComputation();

protected:
    virtual void init() override;
    virtual void draw() override;
    virtual void drawWithNames() override;
    virtual void postSelection(const QPoint &p) override;
    virtual void keyPressEvent(QKeyEvent *e) override;
    virtual void mouseMoveEvent(QMouseEvent *e) override;
    virtual QString helpString() const override;

private:
    struct MyTraits : public OpenMesh::DefaultTraits {
        using Point  = OpenMesh::Vec3d; // the default would be Vec3f
        using Normal = OpenMesh::Vec3d;
        VertexTraits {
            double mean;              // approximated mean curvature
        };
    };
    using MyMesh = OpenMesh::TriMesh_ArrayKernelT<MyTraits>;
    using Vector = OpenMesh::VectorT<double,3>;

    // Mesh
    void updateMesh(bool update_mean_range = true);
    void updateVertexNormals();
#ifdef USE_JET_FITTING
    void updateWithJetFit(size_t neighbors);
#endif
    void localSystem(const Vector &normal, Vector &u, Vector &v);
    double voronoiWeight(MyMesh::HalfedgeHandle in_he);
    void updateMeanMinMax();
    void updateMeanCurvature();

    // Bezier
    static void bernsteinAll(size_t n, double u, std::vector<double> &coeff);
    void generateMesh(size_t resolution);

    // Visualization
    void setupCamera();
    Vec meanMapColor(double d) const;
    void drawControlNet() const;
    void drawAxes() const;
    void drawAxesWithNames() const;
    static Vec intersectLines(const Vec &ap, const Vec &ad, const Vec &bp, const Vec &bd);

    // Other
    void fairMesh();

    //////////////////////
    // Member variables //
    //////////////////////

    enum class ModelType { NONE, MESH, BEZIER_SURFACE } model_type;

    // Mesh
    MyMesh mesh;

    // Bezier
    size_t degree[2];
    std::vector<Vec> control_points;

    // Visualization
    double mean_min, mean_max, cutoff_ratio;
    bool show_control_points, show_solid, show_wireframe;
    enum class Visualization { PLAIN, MEAN, SLICING, ISOPHOTES } visualization;
    GLuint isophote_texture, environment_texture, current_isophote_texture, slicing_texture;
    Vector slicing_dir;
    double slicing_scaling;
    int selected_vertex;
    struct ModificationAxes {
        bool shown;
        float size;
        int selected_axis;
        Vec position, grabbed_pos, original_pos;
    } axes;
    std::string last_filename;

    // Clever Support
    enum locationType {
        COMMON,
        MODEL,
        PLATE
    };
    struct SupportPoint{
        Vec location;
        locationType type;

        SupportPoint(Vec location, enum locationType type): location(location), type(type){}

        bool operator==(const SupportPoint& other) const {
            // Define the equality logic here
            return location == other.location;
        }
    };

    struct TreePoint {

        SupportPoint point;
        SupportPoint nextPoint;

        TreePoint(SupportPoint point, SupportPoint nextPoint): point(point), nextPoint(nextPoint){}
    };

    double gridDensity;
    double angleLimit;
    bool showWhereSupportNeeded;
    bool showAllPoints;
    bool showCones;
    bool showTree;
    std::vector<OpenMesh::SmartVertexHandle> verticesToSupport;
    std::vector<OpenMesh::SmartFaceHandle> facesToSupport;
    std::vector<OpenMesh::SmartEdgeHandle> edgesToSupport;
    std::deque<SupportPoint> pointsToSupport;
    std::vector<TreePoint> treePoints;

public:
    inline double getGridDensity() const;
    inline void setGridDensity(double d);
    inline double getAngleLimit() const;
    inline void setAngleLimit(double a);
    inline void toggleCones();
    inline void toggleTree();
    void colorPointsAndEdges();
    void showAllPointsToSupport();
    void calculatePointsToSupport();
    void generateEdgePoints(Vec A, Vec B, int density);
    void generateFacePoints(OpenMesh::SmartFaceHandle f);
    void generateCones();
    void drawTree();
    void calculateSupportTreePoints();
    SupportPoint getClosestPointFromPoints(SupportPoint p);
    Vec getCommonSupportPoint(Vec p1, Vec p2);
    Vec getClosestPointOnModel(Vec p);
    Vec projectToTriangle(const Vec &p, const OpenMesh::SmartFaceHandle &f);
    void addTreeGeometry();
    void addStrut(SupportPoint top, SupportPoint bottom);
    void addTopConnection(Vec a, Vec b);
    void addFace(Vec v1, Vec v2, Vec v3);
    double degToRad(double deg);
    double angleOfVectors(Vec v1, Vec v2);
    Vec vertexToVec(OpenMesh::SmartVertexHandle v);
    Vec rotateAround(Vec v, Vec pivot, double angle /*radians*/);
    void sortPointsToSupport();
};

#include "MyViewer.hpp"
