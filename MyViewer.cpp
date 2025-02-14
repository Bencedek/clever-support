#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <map>
#include <vector>

#include <QtGui/QKeyEvent>

#include <OpenMesh/Core/IO/MeshIO.hh>
#include <OpenMesh/Core/IO/writer/STLWriter.hh>
#include <OpenMesh/Tools/Smoother/JacobiLaplaceSmootherT.hh>

#ifdef BETTER_MEAN_CURVATURE
#include "Eigen/Eigenvalues"
#include "Eigen/Geometry"
#include "Eigen/LU"
#include "Eigen/SVD"
#endif

#ifdef USE_JET_FITTING
#include "jet-wrapper.h"
#endif

#include "MyViewer.h"

#ifdef _WIN32
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_BGRA 0x80E1
#endif

#include <QDebug>

MyViewer::MyViewer(QWidget *parent) :
    QGLViewer(parent), model_type(ModelType::NONE),
    mean_min(0.0), mean_max(0.0), cutoff_ratio(0.05),
    show_control_points(true), show_solid(true), show_wireframe(false),
    visualization(Visualization::PLAIN), slicing_dir(0, 0, 1), slicing_scaling(1),
    last_filename(""),
    gridDensity(4.0), angleLimit(degToRad(60)), diameterCoefficient(0.07)/* should be 0.0015 as per Vanek (2014)*/, showWhereSupportNeeded(false), showAllPoints(false), showCones(false), showTree(false)
{
    setSelectRegionWidth(10);
    setSelectRegionHeight(10);
    axes.shown = false;

    supportMesh.request_face_normals(); supportMesh.request_halfedge_normals(); supportMesh.request_vertex_normals();
}

MyViewer::~MyViewer() {
    glDeleteTextures(1, &isophote_texture);
    glDeleteTextures(1, &environment_texture);
    glDeleteTextures(1, &slicing_texture);
}

void MyViewer::updateMeanMinMax() {
    size_t n = mesh.n_vertices();
    if (n == 0)
        return;

    std::vector<double> mean;
    mean.reserve(n);
    for (auto v : mesh.vertices())
        mean.push_back(mesh.data(v).mean);

    std::sort(mean.begin(), mean.end());
    size_t k = (double)n * cutoff_ratio;
    mean_min = std::min(mean[k ? k-1 : 0], 0.0);
    mean_max = std::max(mean[k ? n-k : n-1], 0.0);
}

void MyViewer::localSystem(const MyViewer::Vector &normal,
                           MyViewer::Vector &u, MyViewer::Vector &v) {
    // Generates an orthogonal (u,v) coordinate system in the plane defined by `normal`.
    int maxi = 0, nexti = 1;
    double max = std::abs(normal[0]), next = std::abs(normal[1]);
    if (max < next) {
        std::swap(max, next);
        std::swap(maxi, nexti);
    }
    if (std::abs(normal[2]) > max) {
        nexti = maxi;
        maxi = 2;
    } else if (std::abs(normal[2]) > next)
        nexti = 2;

    u.vectorize(0.0);
    u[nexti] = -normal[maxi];
    u[maxi] = normal[nexti];
    u /= u.norm();
    v = normal % u;
}

double MyViewer::voronoiWeight(MyViewer::MyMesh::HalfedgeHandle in_he) {
    // Returns the area of the triangle bounded by in_he that is closest
    // to the vertex pointed to by in_he.
    if (mesh.is_boundary(in_he))
        return 0;
    auto next = mesh.next_halfedge_handle(in_he);
    auto prev = mesh.prev_halfedge_handle(in_he);
    double c2 = mesh.calc_edge_vector(in_he).sqrnorm();
    double b2 = mesh.calc_edge_vector(next).sqrnorm();
    double a2 = mesh.calc_edge_vector(prev).sqrnorm();
    double alpha = mesh.calc_sector_angle(in_he);

    if (a2 + b2 < c2)                // obtuse gamma
        return 0.125 * b2 * std::tan(alpha);
    if (a2 + c2 < b2)                // obtuse beta
        return 0.125 * c2 * std::tan(alpha);
    if (b2 + c2 < a2) {              // obtuse alpha
        double b = std::sqrt(b2), c = std::sqrt(c2);
        double total_area = 0.5 * b * c * std::sin(alpha);
        double beta  = mesh.calc_sector_angle(prev);
        double gamma = mesh.calc_sector_angle(next);
        return total_area - 0.125 * (b2 * std::tan(gamma) + c2 * std::tan(beta));
    }

    double r2 = 0.25 * a2 / std::pow(std::sin(alpha), 2); // squared circumradius
    auto area = [r2](double x2) {
        return 0.125 * std::sqrt(x2) * std::sqrt(std::max(4.0 * r2 - x2, 0.0));
    };
    return area(b2) + area(c2);
}

#ifndef BETTER_MEAN_CURVATURE
void MyViewer::updateMeanCurvature() {
    std::map<MyMesh::FaceHandle, double> face_area;
    std::map<MyMesh::VertexHandle, double> vertex_area;

    for (auto f : mesh.faces())
        face_area[f] = mesh.calc_sector_area(mesh.halfedge_handle(f));

    // Compute triangle strip areas
    for (auto v : mesh.vertices()) {
        vertex_area[v] = 0;
        mesh.data(v).mean = 0;
        for (auto f : mesh.vf_range(v))
            vertex_area[v] += face_area[f];
        vertex_area[v] /= 3.0;
    }

    // Compute mean values using dihedral angles
    for (auto v : mesh.vertices()) {
        for (auto h : mesh.vih_range(v)) {
            auto vec = mesh.calc_edge_vector(h);
            double angle = mesh.calc_dihedral_angle(h); // signed; returns 0 at the boundary
            mesh.data(v).mean += angle * vec.norm();
        }
        mesh.data(v).mean *= 0.25 / vertex_area[v];
    }
}
#else // BETTER_MEAN_CURVATURE
void MyViewer::updateMeanCurvature() {
    // As in the paper:
    //   S. Rusinkiewicz, Estimating curvatures and their derivatives on triangle meshes.
    //     3D Data Processing, Visualization and Transmission, IEEE, 2004.

    std::map<MyMesh::VertexHandle, Vector> efgp; // 2nd principal form
    std::map<MyMesh::VertexHandle, double> wp;   // accumulated weight

    // Initial setup
    for (auto v : mesh.vertices()) {
        efgp[v].vectorize(0.0);
        wp[v] = 0.0;
    }

    for (auto f : mesh.faces()) {
        // Setup local edges, vertices and normals
        auto h0 = mesh.halfedge_handle(f);
        auto h1 = mesh.next_halfedge_handle(h0);
        auto h2 = mesh.next_halfedge_handle(h1);
        auto e0 = mesh.calc_edge_vector(h0);
        auto e1 = mesh.calc_edge_vector(h1);
        auto e2 = mesh.calc_edge_vector(h2);
        auto n0 = mesh.normal(mesh.to_vertex_handle(h1));
        auto n1 = mesh.normal(mesh.to_vertex_handle(h2));
        auto n2 = mesh.normal(mesh.to_vertex_handle(h0));

        Vector n = mesh.normal(f), u, v;
        localSystem(n, u, v);

        // Solve a LSQ equation for (e,f,g) of the face
        Eigen::MatrixXd A(6, 3);
        A << (e0 | u), (e0 | v),    0.0,
            0.0,   (e0 | u), (e0 | v),
            (e1 | u), (e1 | v),    0.0,
            0.0,   (e1 | u), (e1 | v),
            (e2 | u), (e2 | v),    0.0,
            0.0,   (e2 | u), (e2 | v);
        Eigen::VectorXd b(6);
        b << ((n2 - n1) | u),
            ((n2 - n1) | v),
            ((n0 - n2) | u),
            ((n0 - n2) | v),
            ((n1 - n0) | u),
            ((n1 - n0) | v);
        Eigen::Vector3d x = A.fullPivLu().solve(b);

        Eigen::Matrix2d F;          // Fundamental matrix for the face
        F << x(0), x(1),
            x(1), x(2);

        for (auto h : mesh.fh_range(f)) {
            auto p = mesh.to_vertex_handle(h);

            // Rotate the (up,vp) local coordinate system to be coplanar with that of the face
            Vector np = mesh.normal(p), up, vp;
            localSystem(np, up, vp);
            auto axis = (np % n).normalize();
            double angle = std::acos(std::min(std::max(n | np, -1.0), 1.0));
            auto rotation = Eigen::AngleAxisd(angle, Eigen::Vector3d(axis.data()));
            Eigen::Vector3d up1(up.data()), vp1(vp.data());
            up1 = rotation * up1;    vp1 = rotation * vp1;
            up = Vector(up1.data()); vp = Vector(vp1.data());

            // Compute the vertex-local (e,f,g)
            double e, f, g;
            Eigen::Vector2d upf, vpf;
            upf << (up | u), (up | v);
            vpf << (vp | u), (vp | v);
            e = upf.transpose() * F * upf;
            f = upf.transpose() * F * vpf;
            g = vpf.transpose() * F * vpf;

            // Accumulate the results with Voronoi weights
            double w = voronoiWeight(h);
            efgp[p] += Vector(e, f, g) * w;
            wp[p] += w;
        }
    }

    // Compute the principal curvatures
    for (auto v : mesh.vertices()) {
        auto &efg = efgp[v];
        efg /= wp[v];
        Eigen::Matrix2d F;
        F << efg[0], efg[1],
            efg[1], efg[2];
        auto k = F.eigenvalues();   // always real, because F is a symmetric real matrix
        mesh.data(v).mean = (k(0).real() + k(1).real()) / 2.0;
    }
}
#endif

static Vec HSV2RGB(Vec hsv) {
    // As in Wikipedia
    double c = hsv[2] * hsv[1];
    double h = hsv[0] / 60;
    double x = c * (1 - std::abs(std::fmod(h, 2) - 1));
    double m = hsv[2] - c;
    Vec rgb(m, m, m);
    if (h <= 1)
        return rgb + Vec(c, x, 0);
    if (h <= 2)
        return rgb + Vec(x, c, 0);
    if (h <= 3)
        return rgb + Vec(0, c, x);
    if (h <= 4)
        return rgb + Vec(0, x, c);
    if (h <= 5)
        return rgb + Vec(x, 0, c);
    if (h <= 6)
        return rgb + Vec(c, 0, x);
    return rgb;
}

Vec MyViewer::meanMapColor(double d) const {
    double red = 0, green = 120, blue = 240; // Hue
    if (d < 0) {
        double alpha = mean_min ? std::min(d / mean_min, 1.0) : 1.0;
        return HSV2RGB({green * (1 - alpha) + blue * alpha, 1, 1});
    }
    double alpha = mean_max ? std::min(d / mean_max, 1.0) : 1.0;
    return HSV2RGB({green * (1 - alpha) + red * alpha, 1, 1});
}

void MyViewer::fairMesh() {
    if (model_type != ModelType::MESH)
        return;

    emit startComputation(tr("Fairing mesh..."));
    OpenMesh::Smoother::JacobiLaplaceSmootherT<MyMesh> smoother(mesh);
    smoother.initialize(OpenMesh::Smoother::SmootherT<MyMesh>::Normal, // or: Tangential_and_Normal
                        OpenMesh::Smoother::SmootherT<MyMesh>::C1);
    for (size_t i = 1; i <= 10; ++i) {
        smoother.smooth(10);
        emit midComputation(i * 10);
    }
    updateMesh(false);
    emit endComputation();
}

#ifdef USE_JET_FITTING

void MyViewer::updateWithJetFit(size_t neighbors) {
    std::vector<Vector> points;
    for (auto v : mesh.vertices())
        points.push_back(mesh.point(v));

    auto nearest = JetWrapper::Nearest(points, neighbors);

    for (auto v : mesh.vertices()) {
        auto jet = JetWrapper::fit(mesh.point(v), nearest, 2);
        if ((mesh.normal(v) | jet.normal) < 0) {
            mesh.set_normal(v, -jet.normal);
            mesh.data(v).mean = (jet.k_min + jet.k_max) / 2;
        } else {
            mesh.set_normal(v, jet.normal);
            mesh.data(v).mean = -(jet.k_min + jet.k_max) / 2;
        }
    }
}

#endif // USE_JET_FITTING

void MyViewer::updateVertexNormals() {
    // Weights according to:
    //   N. Max, Weights for computing vertex normals from facet normals.
    //     Journal of Graphics Tools, Vol. 4(2), 1999.
    for (auto v : mesh.vertices()) {
        Vector n(0.0, 0.0, 0.0);
        for (auto h : mesh.vih_range(v)) {
            if (mesh.is_boundary(h))
                continue;
            auto in_vec  = mesh.calc_edge_vector(h);
            auto out_vec = mesh.calc_edge_vector(mesh.next_halfedge_handle(h));
            double w = in_vec.sqrnorm() * out_vec.sqrnorm();
            n += (in_vec % out_vec) / (w == 0.0 ? 1.0 : w);
        }
        double len = n.length();
        if (len != 0.0)
            n /= len;
        mesh.set_normal(v, n);
    }
}

void MyViewer::updateMesh(bool update_mean_range) {
    if (model_type == ModelType::BEZIER_SURFACE)
        generateMesh(50);
    mesh.request_face_normals(); mesh.request_halfedge_normals(); mesh.request_vertex_normals();
    mesh.update_face_normals(); mesh.update_halfedge_normals(); mesh.update_vertex_normals();
#ifdef USE_JET_FITTING
    mesh.update_vertex_normals();
    updateWithJetFit(20);
#else // !USE_JET_FITTING
    updateVertexNormals();
    updateMeanCurvature();
#endif
    if (update_mean_range)
        updateMeanMinMax();
}

void MyViewer::setupCamera() {
    // Set camera on the model
    Vector box_min, box_max;
    box_min = box_max = mesh.point(*mesh.vertices_begin());
    for (auto v : mesh.vertices()) {
        box_min.minimize(mesh.point(v));
        box_max.maximize(mesh.point(v));
    }
    camera()->setSceneBoundingBox(Vec(box_min.data()), Vec(box_max.data()));
    camera()->showEntireScene();

    slicing_scaling = 20 / (box_max - box_min).max();

    setSelectedName(-1);
    axes.shown = false;

    update();
}

bool MyViewer::openMesh(const std::string &filename, bool update_view) {
    supportMesh.clear();
    if (!OpenMesh::IO::read_mesh(mesh, filename) || mesh.n_vertices() == 0)
        return false;
    model_type = ModelType::MESH;
    last_filename = filename;
    updateMesh(update_view);
    if (update_view)
        setupCamera();
    return true;
}

bool MyViewer::openBezier(const std::string &filename, bool update_view) {
    size_t n, m;
    try {
        std::ifstream f(filename.c_str());
        f.exceptions(std::ios::failbit | std::ios::badbit);
        f >> n >> m;
        degree[0] = n++; degree[1] = m++;
        control_points.resize(n * m);
        for (size_t i = 0, index = 0; i < n; ++i)
            for (size_t j = 0; j < m; ++j, ++index)
                f >> control_points[index][0] >> control_points[index][1] >> control_points[index][2];
    } catch(std::ifstream::failure &) {
        return false;
    }
    model_type = ModelType::BEZIER_SURFACE;
    last_filename = filename;
    updateMesh(update_view);
    if (update_view)
        setupCamera();
    return true;
}

bool MyViewer::saveMesh(const std::string &filename) {
    if (model_type == ModelType::BEZIER_SURFACE)
        return saveBezier(filename);
    MyMesh combined = mesh;

    emit startComputation(tr("Exporting file"));
    size_t numVerticesInMesh = mesh.n_vertices();
    for (MyMesh::VertexIter v_it = supportMesh.vertices_begin(); v_it != supportMesh.vertices_end(); ++v_it) {
        MyMesh::Point p = supportMesh.point(*v_it);
        combined.add_vertex(p);
    }

    for (MyMesh::FaceIter f_it = supportMesh.faces_begin(); f_it != supportMesh.faces_end(); ++f_it) {
        std::vector<MyMesh::VertexHandle> faceVertices;
        for (MyMesh::FaceVertexIter fv_it = supportMesh.fv_iter(*f_it); fv_it.is_valid(); ++fv_it) {
            MyMesh::VertexHandle v = *fv_it;
            int newIndex = combined.vertex_handle(v.idx()).idx() + numVerticesInMesh;
            faceVertices.push_back(MyMesh::VertexHandle(newIndex));
        }
        combined.add_face(faceVertices);
    }
    emit endComputation();

    return OpenMesh::IO::write_mesh(combined, filename);
}

bool MyViewer::saveBezier(const std::string &filename) {
    try {
        std::ofstream f(filename.c_str());
        f.exceptions(std::ios::failbit | std::ios::badbit);
        f << degree[0] << ' ' << degree[1] << std::endl;
        for (const auto &p : control_points)
            f << p[0] << ' ' << p[1] << ' ' << p[2] << std::endl;
    } catch(std::ifstream::failure &) {
        return false;
    }
    return true;
}

void MyViewer::init() {
    glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, 1);

    QImage img(":/isophotes.png");
    glGenTextures(1, &isophote_texture);
    glBindTexture(GL_TEXTURE_2D, isophote_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, img.width(), img.height(), 0, GL_BGRA,
                 GL_UNSIGNED_BYTE, img.convertToFormat(QImage::Format_ARGB32).bits());

    QImage img2(":/environment.png");
    glGenTextures(1, &environment_texture);
    glBindTexture(GL_TEXTURE_2D, environment_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, img2.width(), img2.height(), 0, GL_BGRA,
                 GL_UNSIGNED_BYTE, img2.convertToFormat(QImage::Format_ARGB32).bits());

    glGenTextures(1, &slicing_texture);
    glBindTexture(GL_TEXTURE_1D, slicing_texture);
    glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    static const unsigned char slicing_img[] = { 0b11111111, 0b00011100 };
    glTexImage1D(GL_TEXTURE_1D, 0, GL_RGB, 2, 0, GL_RGB, GL_UNSIGNED_BYTE_3_3_2, &slicing_img);
}

void MyViewer::draw() {
    if (model_type == ModelType::BEZIER_SURFACE && show_control_points)
        drawControlNet();

    glPolygonMode(GL_FRONT_AND_BACK, !show_solid && show_wireframe ? GL_LINE : GL_FILL);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(1, 1);

    if (show_solid || show_wireframe) {
        if (visualization == Visualization::PLAIN)
            glColor3d(1.0, 1.0, 1.0);
        else if (visualization == Visualization::ISOPHOTES) {
            glBindTexture(GL_TEXTURE_2D, current_isophote_texture);
            glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_DECAL);
            glEnable(GL_TEXTURE_2D);
            glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_SPHERE_MAP);
            glTexGeni(GL_T, GL_TEXTURE_GEN_MODE, GL_SPHERE_MAP);
            glEnable(GL_TEXTURE_GEN_S);
            glEnable(GL_TEXTURE_GEN_T);
        } else if (visualization == Visualization::SLICING) {
            glBindTexture(GL_TEXTURE_1D, slicing_texture);
            glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_DECAL);
            glEnable(GL_TEXTURE_1D);
        }
        for (auto f : mesh.faces()) {
            glBegin(GL_POLYGON);
            for (auto v : mesh.fv_range(f)) {
                if (visualization == Visualization::MEAN)
                    glColor3dv(meanMapColor(mesh.data(v).mean));
                else if (visualization == Visualization::SLICING)
                    glTexCoord1d(mesh.point(v) | slicing_dir * slicing_scaling);
                glNormal3dv(mesh.normal(v).data());
                glVertex3dv(mesh.point(v).data());
            }
            glEnd();
        }
        if (visualization == Visualization::ISOPHOTES) {
            glDisable(GL_TEXTURE_GEN_S);
            glDisable(GL_TEXTURE_GEN_T);
            glDisable(GL_TEXTURE_2D);
            glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
        } else if (visualization == Visualization::SLICING) {
            glDisable(GL_TEXTURE_1D);
        }
    }

    if (show_solid && show_wireframe) {
        glPolygonMode(GL_FRONT, GL_LINE);
        glColor3d(0.0, 0.0, 0.0);
        glDisable(GL_LIGHTING);
        for (auto f : mesh.faces()) {
            glBegin(GL_POLYGON);
            for (auto v : mesh.fv_range(f))
                glVertex3dv(mesh.point(v).data());
            glEnd();
        }
        glEnable(GL_LIGHTING);
    }

    // for Clever Support
    if (showWhereSupportNeeded) {
        colorFacesEdgesAndPoints();
        if (showCones) {
            generateCones();
        }
    }
    if (showTree){
        drawTree();
    }
    for (auto f : supportMesh.faces()) {
        glColor3d(1.0, 0.5, 0.0);
        glBegin(GL_POLYGON);
        for (auto v : supportMesh.fv_range(f)) {
            glNormal3dv(supportMesh.normal(v).data());
            glVertex3dv(supportMesh.point(v).data());
        }

        glEnd();
    }

    if (axes.shown)
        drawAxes();
}

void MyViewer::drawControlNet() const {
    glDisable(GL_LIGHTING);
    glLineWidth(3.0);
    glColor3d(0.3, 0.3, 1.0);
    size_t m = degree[1] + 1;
    for (size_t k = 0; k < 2; ++k)
        for (size_t i = 0; i <= degree[k]; ++i) {
            glBegin(GL_LINE_STRIP);
            for (size_t j = 0; j <= degree[1-k]; ++j) {
                size_t const index = k ? j * m + i : i * m + j;
                const auto &p = control_points[index];
                glVertex3dv(p);
            }
            glEnd();
        }
    glLineWidth(1.0);
    glPointSize(8.0);
    glColor3d(1.0, 0.0, 1.0);
    glBegin(GL_POINTS);
    for (const auto &p : control_points)
        glVertex3dv(p);
    glEnd();
    glPointSize(1.0);
    glEnable(GL_LIGHTING);
}

void MyViewer::drawAxes() const {
    const Vec &p = axes.position;
    glColor3d(1.0, 0.0, 0.0);
    drawArrow(p, p + Vec(axes.size, 0.0, 0.0), axes.size / 50.0);
    glColor3d(0.0, 1.0, 0.0);
    drawArrow(p, p + Vec(0.0, axes.size, 0.0), axes.size / 50.0);
    glColor3d(0.0, 0.0, 1.0);
    drawArrow(p, p + Vec(0.0, 0.0, axes.size), axes.size / 50.0);
    glEnd();
}

void MyViewer::drawWithNames() {
    if (axes.shown)
        return drawAxesWithNames();

    switch (model_type) {
    case ModelType::NONE: break;
    case ModelType::MESH:
        if (!show_wireframe)
            return;
        for (auto v : mesh.vertices()) {
            glPushName(v.idx());
            glRasterPos3dv(mesh.point(v).data());
            glPopName();
        }
        break;
    case ModelType::BEZIER_SURFACE:
        if (!show_control_points)
            return;
        for (size_t i = 0, ie = control_points.size(); i < ie; ++i) {
            Vec const &p = control_points[i];
            glPushName(i);
            glRasterPos3fv(p);
            glPopName();
        }
        break;
    }
}

void MyViewer::drawAxesWithNames() const {
    const Vec &p = axes.position;
    glPushName(0);
    drawArrow(p, p + Vec(axes.size, 0.0, 0.0), axes.size / 50.0);
    glPopName();
    glPushName(1);
    drawArrow(p, p + Vec(0.0, axes.size, 0.0), axes.size / 50.0);
    glPopName();
    glPushName(2);
    drawArrow(p, p + Vec(0.0, 0.0, axes.size), axes.size / 50.0);
    glPopName();
}

void MyViewer::postSelection(const QPoint &p) {
    int sel = selectedName();
    if (sel == -1) {
        axes.shown = false;
        return;
    }

    if (axes.shown) {
        axes.selected_axis = sel;
        bool found;
        axes.grabbed_pos = camera()->pointUnderPixel(p, found);
        axes.original_pos = axes.position;
        if (!found)
            axes.shown = false;
        return;
    }

    selected_vertex = sel;
    if (model_type == ModelType::MESH)
        axes.position = Vec(mesh.point(MyMesh::VertexHandle(sel)).data());
    if (model_type == ModelType::BEZIER_SURFACE)
        axes.position = control_points[sel];
    double depth = camera()->projectedCoordinatesOf(axes.position)[2];
    Vec q1 = camera()->unprojectedCoordinatesOf(Vec(0.0, 0.0, depth));
    Vec q2 = camera()->unprojectedCoordinatesOf(Vec(width(), height(), depth));
    axes.size = (q1 - q2).norm() / 10.0;
    axes.shown = true;
    axes.selected_axis = -1;
}

void MyViewer::keyPressEvent(QKeyEvent *e) {
    if (e->modifiers() == Qt::NoModifier)
        switch (e->key()) {
        case Qt::Key_R:
            if (model_type == ModelType::MESH)
                openMesh(last_filename, false);
            else if (model_type == ModelType::BEZIER_SURFACE)
                openBezier(last_filename, false);
            update();
            break;
        case Qt::Key_O:
            if (camera()->type() == qglviewer::Camera::PERSPECTIVE)
                camera()->setType(qglviewer::Camera::ORTHOGRAPHIC);
            else
                camera()->setType(qglviewer::Camera::PERSPECTIVE);
            update();
            break;
        case Qt::Key_P:
            visualization = Visualization::PLAIN;
            update();
            break;
        case Qt::Key_M:
            visualization = Visualization::MEAN;
            update();
            break;
        case Qt::Key_L:
            visualization = Visualization::SLICING;
            update();
            break;
        case Qt::Key_I:
            visualization = Visualization::ISOPHOTES;
            current_isophote_texture = isophote_texture;
            update();
            break;
        case Qt::Key_E:
            visualization = Visualization::ISOPHOTES;
            current_isophote_texture = environment_texture;
            update();
            break;
        case Qt::Key_C:
            show_control_points = !show_control_points;
            update();
            break;
        case Qt::Key_S:
            show_solid = !show_solid;
            update();
            break;
        case Qt::Key_W:
            show_wireframe = !show_wireframe;
            update();
            break;
        case Qt::Key_F:
            fairMesh();
            update();
            break;
        case Qt::Key_X:
            showWhereSupportNeeded = !showWhereSupportNeeded;
            update();
            break;
        default:
            QGLViewer::keyPressEvent(e);
        }
    else if (e->modifiers() == Qt::KeypadModifier)
        switch (e->key()) {
        case Qt::Key_Plus:
            slicing_scaling *= 2;
            update();
            break;
        case Qt::Key_Minus:
            slicing_scaling /= 2;
            update();
            break;
        case Qt::Key_Asterisk:
            slicing_dir = Vector(static_cast<double *>(camera()->viewDirection()));
            update();
            break;
        }
    else if (e->modifiers() == Qt::AltModifier)
        switch (e->key()) {
        case Qt::Key_X:
            showAllPoints = !showAllPoints;
            update();
            break;
        }
    else
        QGLViewer::keyPressEvent(e);
}

Vec MyViewer::intersectLines(const Vec &ap, const Vec &ad, const Vec &bp, const Vec &bd) {
    // always returns a point on the (ap, ad) line
    double a = ad * ad, b = ad * bd, c = bd * bd;
    double d = ad * (ap - bp), e = bd * (ap - bp);
    if (a * c - b * b < 1.0e-7)
        return ap;
    double s = (b * e - c * d) / (a * c - b * b);
    return ap + s * ad;
}

void MyViewer::bernsteinAll(size_t n, double u, std::vector<double> &coeff) {
    coeff.clear(); coeff.reserve(n + 1);
    coeff.push_back(1.0);
    double u1 = 1.0 - u;
    for (size_t j = 1; j <= n; ++j) {
        double saved = 0.0;
        for (size_t k = 0; k < j; ++k) {
            double tmp = coeff[k];
            coeff[k] = saved + tmp * u1;
            saved = tmp * u;
        }
        coeff.push_back(saved);
    }
}

void MyViewer::generateMesh(size_t resolution) {
    mesh.clear();
    std::vector<MyMesh::VertexHandle> handles, tri;
    size_t n = degree[0], m = degree[1];

    std::vector<double> coeff_u, coeff_v;
    for (size_t i = 0; i < resolution; ++i) {
        double u = (double)i / (double)(resolution - 1);
        bernsteinAll(n, u, coeff_u);
        for (size_t j = 0; j < resolution; ++j) {
            double v = (double)j / (double)(resolution - 1);
            bernsteinAll(m, v, coeff_v);
            Vec p(0.0, 0.0, 0.0);
            for (size_t k = 0, index = 0; k <= n; ++k)
                for (size_t l = 0; l <= m; ++l, ++index)
                    p += control_points[index] * coeff_u[k] * coeff_v[l];
            handles.push_back(mesh.add_vertex(Vector(static_cast<double *>(p))));
        }
    }
    for (size_t i = 0; i < resolution - 1; ++i)
        for (size_t j = 0; j < resolution - 1; ++j) {
            tri.clear();
            tri.push_back(handles[i * resolution + j]);
            tri.push_back(handles[i * resolution + j + 1]);
            tri.push_back(handles[(i + 1) * resolution + j]);
            mesh.add_face(tri);
            tri.clear();
            tri.push_back(handles[(i + 1) * resolution + j]);
            tri.push_back(handles[i * resolution + j + 1]);
            tri.push_back(handles[(i + 1) * resolution + j + 1]);
            mesh.add_face(tri);
        }
}

void MyViewer::mouseMoveEvent(QMouseEvent *e) {
    if (!axes.shown ||
        (axes.selected_axis < 0 && !(e->modifiers() & Qt::ControlModifier)) ||
        !(e->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier)) ||
        !(e->buttons() & Qt::LeftButton))
        return QGLViewer::mouseMoveEvent(e);

    if (e->modifiers() & Qt::ControlModifier) {
        // move in screen plane
        double depth = camera()->projectedCoordinatesOf(axes.position)[2];
        axes.position = camera()->unprojectedCoordinatesOf(Vec(e->pos().x(), e->pos().y(), depth));
    } else {
        Vec from, dir, axis(axes.selected_axis == 0, axes.selected_axis == 1, axes.selected_axis == 2);
        camera()->convertClickToLine(e->pos(), from, dir);
        auto p = intersectLines(axes.grabbed_pos, axis, from, dir);
        float d = (p - axes.grabbed_pos) * axis;
        axes.position[axes.selected_axis] = axes.original_pos[axes.selected_axis] + d;
    }

    if (model_type == ModelType::MESH)
        mesh.set_point(MyMesh::VertexHandle(selected_vertex),
                       Vector(static_cast<double *>(axes.position)));
    if (model_type == ModelType::BEZIER_SURFACE)
        control_points[selected_vertex] = axes.position;
    updateMesh();
    update();
}

QString MyViewer::helpString() const {
    QString text("<h2>Sample Framework</h2>"
                 "<p>This is a minimal framework for 3D mesh manipulation, which can be "
                 "extended and used as a base for various projects, for example "
                 "prototypes for fairing algorithms, or even displaying/modifying "
                 "parametric surfaces, etc.</p>"
                 "<p>The following hotkeys are available:</p>"
                 "<ul>"
                 "<li>&nbsp;R: Reload model</li>"
                 "<li>&nbsp;O: Toggle orthographic projection</li>"
                 "<li>&nbsp;P: Set plain map (no coloring)</li>"
                 "<li>&nbsp;M: Set mean curvature map</li>"
                 "<li>&nbsp;L: Set slicing map<ul>"
                 "<li>&nbsp;+: Increase slicing density</li>"
                 "<li>&nbsp;-: Decrease slicing density</li>"
                 "<li>&nbsp;*: Set slicing direction to view</li></ul></li>"
                 "<li>&nbsp;I: Set isophote line map</li>"
                 "<li>&nbsp;E: Set environment texture</li>"
                 "<li>&nbsp;C: Toggle control polygon visualization</li>"
                 "<li>&nbsp;S: Toggle solid (filled polygon) visualization</li>"
                 "<li>&nbsp;W: Toggle wireframe visualization</li>"
                 "<li>&nbsp;F: Fair mesh</li>"
                 "</ul>"
                 "<p>There is also a simple selection and movement interface, enabled "
                 "only when the wireframe/controlnet is displayed: a mesh vertex can be selected "
                 "by shift-clicking, and it can be moved by shift-dragging one of the "
                 "displayed axes. Pressing ctrl enables movement in the screen plane.</p>"
                 "<p>Note that libQGLViewer is furnished with a lot of useful features, "
                 "such as storing/loading view positions, or saving screenshots. "
                 "OpenMesh also has a nice collection of tools for mesh manipulation: "
                 "decimation, subdivision, smoothing, etc. These can provide "
                 "good comparisons to the methods you implement.</p>"
                 "<p>This software can be used as a sample GUI base for handling "
                 "parametric or procedural surfaces, as well. The power of "
                 "Qt and libQGLViewer makes it easy to set up a prototype application. "
                 "Feel free to modify and explore!</p>"
                 "<p align=\"right\">Peter Salvi</p>");
    return text;
}

// Clever Support

void MyViewer::colorFacesEdgesAndPoints(){
    getElementsThatNeedSupport();

    // draw faces
    glPolygonMode(GL_FRONT_AND_BACK, !show_solid && show_wireframe ? GL_LINE : GL_FILL);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glColor3d(1.0, 0.0, 0.0);
    for(auto f : facesToSupport){
        glBegin(GL_POLYGON);
        for(auto v : mesh.fv_range(f)){
            glNormal3dv(mesh.normal(v).data());
            glVertex3dv(mesh.point(v).data());
        }
        glEnd();
    }

    // draw edges
    glPolygonMode(GL_FRONT, GL_LINE);
    glColor3d(0.0, 1.0, 0.0);
    glLineWidth(2.0);
    glDisable(GL_LIGHTING);
    glBegin(GL_LINES);
    for (auto e : edgesToSupport){
        glVertex3dv(mesh.point(e.v0()).data());
        glVertex3dv(mesh.point(e.v1()).data());
    }
    glEnd();
    glLineWidth(1.0);

    // draw points
    if (showAllPoints) {
        showAllPointsToSupport();
    }
    else {
        glPolygonMode(GL_FRONT, GL_POINT);
        glColor3d(1.0, 0.0, 1.0);
        glPointSize(5.0);
        glBegin(GL_POINTS);
        for (auto v : verticesToSupport){
            glVertex3dv(mesh.point(v).data());
        }
        glEnd();
        glPointSize(1.0);
        glEnable(GL_LIGHTING);
    }
}

void MyViewer::getElementsThatNeedSupport(){
    facesToSupport.clear();
    edgesToSupport.clear();
    verticesToSupport.clear();

    for(auto f : mesh.faces()){
        if (angleOfVectors(Vec(mesh.normal(f).data()), Vec(0,0,1)) - degToRad(90.0) >= angleLimit){
            facesToSupport.push_back(f);
        }
    }

    for (auto v : mesh.vertices()) {
        OpenMesh::SmartVertexHandle* lowestOfNeighbors = &v;
        float lowestZ = mesh.point(v).data()[2];
        std::vector<OpenMesh::SmartVertexHandle> equals;
        for (auto vn : v.vertices()){
            float vnZ = mesh.point(vn).data()[2];
            if (vnZ < lowestZ){
                lowestZ = vnZ;
                lowestOfNeighbors = &vn;
                equals.clear();
            } else if (vnZ == lowestZ){
                equals.push_back(vn);
            }
        }
        if (lowestOfNeighbors == &v && Vec(mesh.normal(v).data()).z < 0){
            if (equals.empty()){
                verticesToSupport.push_back(v);
            }
            else if (equals.size() == 1){
                for (auto e : mesh.edges()){
                    if ((e.v0() == v && e.v1() == equals.back()) || (e.v1() == v && e.v0() == equals.back())){
                        if (std::find(edgesToSupport.begin(), edgesToSupport.end(), e) == edgesToSupport.end()) {
                            edgesToSupport.push_back(e);
                        }
                    }
                }
            }
        }
    }
}

void MyViewer::showAllPointsToSupport(){
    calculatePointsToSupport();

    glPolygonMode(GL_FRONT, GL_POINT);
    glColor3d(1.0, 0.0, 1.0);
    glPointSize(5.0);
    glBegin(GL_POINTS);
    for (auto p : pointsToSupport){
        glVertex3dv(p.location);
    }
    glEnd();
    glPointSize(1.0);
    glEnable(GL_LIGHTING);
}

void MyViewer::calculatePointsToSupport(){
    pointsToSupport.clear();

    for (auto v: verticesToSupport){
        pointsToSupport.push_back(SupportPoint(vertexToVec(v), MODEL, Vec(mesh.normal(v).data())));
    }
    for (auto e : edgesToSupport){
        MyMesh::Normal edgeNormal = (mesh.normal(e.h0()) + mesh.normal(e.h1())).normalize();
        generateEdgePoints(vertexToVec(e.v0()), vertexToVec(e.v1()), gridDensity, Vec(edgeNormal.data()));
    }
    for (auto f : facesToSupport){
        generateFacePoints(f);
    }
    sortPointsToSupport();
    pointsToSupport.erase(std::unique( pointsToSupport.begin(), pointsToSupport.end() ), pointsToSupport.end());
}

void MyViewer::generateEdgePoints(Vec A, Vec B, int density, Vec normal){
    Vec v(A - B);

    for(size_t i = 0; i < density; ++i){
        pointsToSupport.push_back(SupportPoint(B + i * (v / (density - 1)), MODEL, normal));
    }
}

void MyViewer::generateFacePoints(OpenMesh::SmartFaceHandle f){
    std::vector<Vec> vertices;
    for (auto v : f.vertices()){
        vertices.push_back(vertexToVec(v));
    }
    Vec A(vertices[0]);
    Vec B(vertices[1]);
    Vec C(vertices[2]);

    Vec v1 = A - B;
    Vec v2 = C - B;

    for(int i = gridDensity; i > 1; --i){
        double delta = (i-1) / (gridDensity-1);
        generateEdgePoints(B + v1 * delta, B + v2 * delta, i, Vec(mesh.normal(f).data()));
    }
    pointsToSupport.push_back(SupportPoint(B, MODEL, Vec(mesh.normal(f).data())));
}

void MyViewer::generateCones(){
    for (auto p : pointsToSupport) {
        std::vector<Vec> coneBasePoints;
        for (int i = 0; i < 50; ++i) {
            double pivotAngle = i * 2 * M_PI / 50;
            coneBasePoints.push_back(Vec(p.location.x + cos(pivotAngle) * tan(angleLimit) * p.location.z, p.location.y + sin(pivotAngle) * tan(angleLimit) * p.location.z, 0.0));
        }
        glDisable(GL_LIGHTING);
        glPolygonMode(GL_FRONT, GL_LINES);
        glColor3d(1.0, 1.0, 0.0);
        glBegin(GL_LINES);
        for (auto s : coneBasePoints){
            glVertex3dv(p.location);
            glVertex3dv(s);
        }
        glEnd();
        glEnable(GL_LIGHTING);
    }
}

void MyViewer::drawTree(){
    if (treePoints.empty()) calculateSupportTreePoints();
    glDisable(GL_LIGHTING);
    glPolygonMode(GL_FRONT, GL_LINES);
    glLineWidth(2.0);
    glColor3d(0.0, 1.0, 1.0);
    glBegin(GL_LINES);
    for (auto tp : treePoints){
        glVertex3dv(tp.point.location);
        glVertex3dv(tp.nextPoint.location);
    }
    glEnd();
    glLineWidth(1.0);
    glEnable(GL_LIGHTING);
}

void MyViewer::calculateSupportTreePoints(){
    treePoints.clear();
    getElementsThatNeedSupport();
    calculatePointsToSupport();
    double lowestZ;
    if (!pointsToSupport.empty()) lowestZ  = pointsToSupport.back().location.z;
    double fullSize = pointsToSupport.size() * 2;
    int cnt = 0;
    emit startComputation(tr("Calculating tree points..."));

    while(!pointsToSupport.empty()){
        emit midComputation(100 * (++cnt / fullSize));
        SupportPoint p = pointsToSupport.front();
        if (p.location.z > lowestZ){
            SupportPoint closestFromPoints = getClosestPointFromPoints(p);
            SupportPoint closestOnModel = getClosestPointOnModel(p);
            Vec closestOnBase (p.location.x, p.location.y, lowestZ);
            double distanceFromClosest = (p.location - closestFromPoints.location).norm();
            double distanceFromModel = (p.location - closestOnModel.location).norm();
            double distanceFromBase = (p.location - closestOnBase).norm();
            Vec closest;

            if (distanceFromClosest > 0.0 && distanceFromModel > 0.0){
                if (distanceFromClosest < distanceFromBase && distanceFromClosest <= distanceFromModel) closest = closestFromPoints.location;
                else if (distanceFromModel < distanceFromClosest && distanceFromModel < distanceFromBase) closest = closestOnModel.location;
                else closest = closestOnBase;
            } else if (distanceFromClosest == 0.0 && distanceFromModel > 0.0) {
                if (distanceFromModel < distanceFromBase) closest = closestOnModel.location;
                else closest = closestOnBase;
            }  else if (distanceFromModel == 0.0 && distanceFromClosest > 0.0) {
                if (distanceFromClosest < distanceFromBase) closest = closestFromPoints.location;
                else closest = closestOnBase;
            } else closest = closestOnBase;

            if (p.type == locationType::MODEL){
                if (p.location.z - lowestZ < 1.0) treePoints.push_back(TreePoint(p, SupportPoint(closestOnBase, COMMON)));
                else treePoints.push_back(TreePoint(p, SupportPoint(p.location + p.normal.unit(), COMMON)));
                pointsToSupport.push_back(SupportPoint(p.location + p.normal.unit(), COMMON));
            } else if (pointsToSupport.size() > 1 && closest == closestFromPoints.location && closest != p.location)
            {
                Vec common = getCommonSupportPoint(p.location, closestFromPoints.location);
                treePoints.push_back(TreePoint(p, SupportPoint(common, COMMON)));
                treePoints.push_back(TreePoint(closestFromPoints, SupportPoint(common, COMMON)));
                pointsToSupport.erase(std::find(pointsToSupport.begin(), pointsToSupport.end(), closestFromPoints));
                pointsToSupport.push_back(SupportPoint(common, COMMON));
            } else if (closest == closestOnModel.location && closest != p.location){
                /*SupportPoint midpoint(closestOnModel.location - (closestOnModel.location-p.location).unit(), COMMON);
                treePoints.push_back(TreePoint(p, midpoint));
                treePoints.push_back(TreePoint(midpoint, closestOnModel));*/
                treePoints.push_back(TreePoint(p, closestOnModel));
            } else {
                treePoints.push_back(TreePoint(p, SupportPoint(closest, PLATE)));
            }
        }
        pointsToSupport.pop_front();
        sortPointsToSupport();
    }
    emit endComputation();
    update();
}

MyViewer::SupportPoint MyViewer::getClosestPointFromPoints(SupportPoint p){
    if (pointsToSupport.size() <= 1)
        return p;
    else {
        SupportPoint closest = pointsToSupport[1];
        for(size_t i = 1; i < pointsToSupport.size(); ++i){
            if ( (pointsToSupport[i].location - p.location).norm() < (closest.location - p.location).norm()
                && angleOfVectors(pointsToSupport[i].location - p.location, Vec(pointsToSupport[i].location.x, pointsToSupport[i].location.y, p.location.z) - p.location) < degToRad(90) - angleLimit)
                closest = pointsToSupport[i];
        }
        if (angleOfVectors(closest.location - p.location, Vec(closest.location.x, closest.location.y, p.location.z) - p.location) > degToRad(90) - angleLimit)
            return p;
        return closest;
    }
}

Vec MyViewer::getCommonSupportPoint(Vec p1, Vec p2){
    Vec normal = ((p2 - p1).unit() ^ Vec(0.0, 0.0, 1.0)).unit();
    Vec fromp1 = rotateAround(Vec(p1.x, p1.y, 0.0) - p1, normal, angleLimit);
    Vec fromp2 = rotateAround(Vec(p2.x, p2.y, 0.0) - p2, normal, -angleLimit);
    return intersectLines(p1, fromp1, p2, fromp2);
}

MyViewer::SupportPoint MyViewer::getClosestPointOnModel(MyViewer::SupportPoint p){
    Vec closest;
    bool closestSet = false;
    Vec normal;
    for(auto f: mesh.faces()){
            Vec projection = projectToTriangle(p.location, f);
            if ( projection.z < p.location.z
                && angleOfVectors(projection - p.location, Vec(projection.x, projection.y, p.location.z) - p.location) > degToRad(90)-angleLimit
                && (!closestSet
                    || (projection - p.location).norm() < (closest - p.location).norm())){
                closest = projection;
                normal = Vec(mesh.normal(f).data());
                closestSet = true;
            }
    }
    if (closestSet) return SupportPoint(closest, MODEL, normal);
    return p;
}

Vec MyViewer::projectToTriangle(const Vec &p, const OpenMesh::SmartFaceHandle &f) {
    std::vector<Vec> vertices;
    for (auto v : f.vertices()){
        vertices.push_back(vertexToVec(v));
    }
    const Vec &q1 = vertices[0], &q2 = vertices[1], &q3 = vertices[2];
    // As in Schneider, Eberly: Geometric Tools for Computer Graphics, Morgan Kaufmann, 2003.
    // Section 10.3.2, pp. 376-382 (with my corrections)
    const Vec &P = p, &B = q1;
    Vec E0 = q2 - q1, E1 = q3 - q1, D = B - P;
    double a = E0 * E0, b = E0 * E1, c = E1 * E1, d = E0 * D, e = E1 * D;
    double det = a * c - b * b, s = b * e - c * d, t = b * d - a * e;
    if (s + t <= det) {
        if (s < 0) {
            if (t < 0) {
                // Region 4
                if (e < 0) {
                    s = 0.0;
                    t = (-e >= c ? 1.0 : -e / c);
                } else if (d < 0) {
                    t = 0.0;
                    s = (-d >= a ? 1.0 : -d / a);
                } else {
                    s = 0.0;
                    t = 0.0;
                }
            } else {
                // Region 3
                s = 0.0;
                t = (e >= 0.0 ? 0.0 : (-e >= c ? 1.0 : -e / c));
            }
        } else if (t < 0) {
            // Region 5
            t = 0.0;
            s = (d >= 0.0 ? 0.0 : (-d >= a ? 1.0 : -d / a));
        } else {
            // Region 0
            double invDet = 1.0 / det;
            s *= invDet;
            t *= invDet;
        }
    } else {
        if (s < 0) {
            // Region 2
            double tmp0 = b + d, tmp1 = c + e;
            if (tmp1 > tmp0) {
                double numer = tmp1 - tmp0;
                double denom = a - 2 * b + c;
                s = (numer >= denom ? 1.0 : numer / denom);
                t = 1.0 - s;
            } else {
                s = 0.0;
                t = (tmp1 <= 0.0 ? 1.0 : (e >= 0.0 ? 0.0 : -e / c));
            }
        } else if (t < 0) {
            // Region 6
            double tmp0 = b + e, tmp1 = a + d;
            if (tmp1 > tmp0) {
                double numer = tmp1 - tmp0;
                double denom = c - 2 * b + a;
                t = (numer >= denom ? 1.0 : numer / denom);
                s = 1.0 - t;
            } else {
                t = 0.0;
                s = (tmp1 <= 0.0 ? 1.0 : (d >= 0.0 ? 0.0 : -d / a));
            }
        } else {
            // Region 1
            double numer = c + e - b - d;
            if (numer <= 0) {
                s = 0;
            } else {
                double denom = a - 2 * b + c;
                s = (numer >= denom ? 1.0 : numer / denom);
            }
            t  = 1.0 - s;
        }
    }
    return B + E0 * s + E1 * t;
}

void MyViewer::addTreeGeometry(){
    showWhereSupportNeeded = false;
    update();
    if (treePoints.empty()) calculateSupportTreePoints();
    supportMesh.clear();
    emit startComputation(tr("Generating tree..."));
    double counter = 0.0;
    for(auto t : treePoints){
        emit midComputation(100 * counter/treePoints.size());
        if (t.point.location != t.nextPoint.location) addStrut(t.point, t.nextPoint);
        counter += 1.0;
    }
    supportMesh.update_normals();
    emit endComputation();
}



void MyViewer::addStrut(SupportPoint top, SupportPoint bottom){
    Vec topPoint = top.location;
    Vec bottomPoint = bottom.location;
    double length = (top.location - bottom.location).norm();
    double r = (diameterCoefficient * length * (angleOfVectors(top.location - bottom.location, Vec(0,0,1)) == 0 ? 1 : angleOfVectors(top.location - bottom.location, Vec(0,0,1))));
    //double r = (diameterCoefficient * (topPoint - bottomPoint).norm() * (1 - angleOfVectors(topPoint-bottomPoint, Vec(0,0,1))));
    if (r < 1) r = 1;
    std::vector<Vec> topTriangle, bottomTriangle;
    for(int i = 0; i < 3; ++i){
        Vec newPoint = rotateAround(Vec(r, 0.0, 0.0), Vec(0.0, 0.0, 1.0), i * 2 * M_PI / 3);
        /*if(top.type == MODEL){
            Vec perp = top.normal ^ Vec(1.0, 0.0, 0.0);
            Vec topConnectionPoint = rotateAround(perp, top.normal, i * 2 * M_PI / 3) * r/2;
            topTriangle.push_back(topPoint + topConnectionPoint);
        } else {*/
            topTriangle.push_back(topPoint + newPoint);
        //}
        if(bottom.type == MODEL){
            Vec perp = bottom.normal.unit() * r ^ Vec(1.0, 0.0, 0.0);
            double distance = r / perp.norm();
            Vec bottomConnectionPoint = rotateAround(perp * distance, bottom.normal, i * 2 * M_PI / 3);
            bottomTriangle.push_back(bottomPoint + bottomConnectionPoint);
        } else {
            bottomTriangle.push_back(bottomPoint + newPoint);
        }
    }


    if (top.type == MODEL){
        /*addFace(topTriangle[0], bottomTriangle[2], bottomTriangle[0]);
        addFace(topTriangle[0], bottomTriangle[0], topTriangle[2]);
        addFace(topTriangle[2], bottomTriangle[0], bottomTriangle[1]);
        addFace(topTriangle[2], bottomTriangle[1], topTriangle[1]);
        addFace(topTriangle[1], bottomTriangle[1], bottomTriangle[2]);
        addFace(topTriangle[1], bottomTriangle[2], topTriangle[0]);*/
        addFace(top.location, bottomTriangle[0], bottomTriangle[1]);
        addFace(top.location, bottomTriangle[1], bottomTriangle[2]);
        addFace(top.location, bottomTriangle[2], bottomTriangle[0]);
    } else {
        addFace(topTriangle[0], topTriangle[1], topTriangle[2]);

        if (bottom.type == MODEL){
            addFace(topTriangle[0], bottomTriangle[1], bottomTriangle[2]);
            addFace(topTriangle[0], bottomTriangle[2], topTriangle[1]);
            addFace(topTriangle[1], bottomTriangle[2], bottomTriangle[0]);
            addFace(topTriangle[1], bottomTriangle[0], topTriangle[2]);
            addFace(topTriangle[2], bottomTriangle[0], bottomTriangle[1]);
            addFace(topTriangle[2], bottomTriangle[1], topTriangle[0]);
        }
        else {
            addFace(topTriangle[0], bottomTriangle[0], bottomTriangle[1]);
            addFace(topTriangle[0], bottomTriangle[1], topTriangle[1]);
            addFace(topTriangle[1], bottomTriangle[1], bottomTriangle[2]);
            addFace(topTriangle[1], bottomTriangle[2], topTriangle[2]);
            addFace(topTriangle[2], bottomTriangle[2], bottomTriangle[0]);
            addFace(topTriangle[2], bottomTriangle[0], topTriangle[0]);
            addFace(bottomTriangle[2], bottomTriangle[1], bottomTriangle[0]);
        }
    }
}

void MyViewer::addFace(Vec v1, Vec v2, Vec v3){
    MyMesh::VertexHandle vh1 = supportMesh.add_vertex(MyMesh::Point(v1.v_));
    MyMesh::VertexHandle vh2 = supportMesh.add_vertex(MyMesh::Point(v2.v_));
    MyMesh::VertexHandle vh3 = supportMesh.add_vertex(MyMesh::Point(v3.v_));
    std::vector<MyMesh::VertexHandle> faceVertices;
    faceVertices.push_back(vh1);
    faceVertices.push_back(vh2);
    faceVertices.push_back(vh3);
    supportMesh.add_face(faceVertices);
}

double MyViewer::degToRad(double deg){
    return deg * M_PI / 180;
}

double MyViewer::angleOfVectors(Vec v1, Vec v2){
    return acos(v1 * v2 / (v1.norm() * v2.norm()));
}

Vec MyViewer::vertexToVec(OpenMesh::SmartVertexHandle v){
    auto vtxdata = mesh.point(v).data();
    return Vec(vtxdata[0], vtxdata[1], vtxdata[2]);
}

Vec MyViewer::rotateAround(Vec v, Vec pivot, double angle){
    return v * cos(angle) + (pivot ^ v) * sin(angle) + pivot * (pivot * v) * (1 - cos(angle)); // Rodrigues' rotation formula
}

void MyViewer::sortPointsToSupport(){
    std::sort(pointsToSupport.begin(), pointsToSupport.end(), [](SupportPoint a, SupportPoint b) {
        if (a.location.z == b.location.z) return a.location.x+a.location.y > b.location.x+b.location.y;
        else return a.location.z > b.location.z;});
}
