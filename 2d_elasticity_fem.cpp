#include "inmost.h"

//    !!!!!!! Currently NOT suited for parallel run
//
//
//    This code solves the following
//    boundary value problem for elastic deformation
//
//    -div(sigma) = f                       | in unit square
//    U          = g                        | on boundary
//    sigma      = C*eps                    | Hooke's law
//    eps        = (grad(U) + grad^T(U))/2  | d
//
//    Here:
//    - sigma is stress tensor, a 2x2 matrix
//    - eps   is strain tensor, a 2x2 matrix
//    - U     is displacement vector of size 2
//    - C     is 4th order elastic tensor of the form
//      [ 2*mu+lam        lam     0 ]
//      [      lam   2*mu+lam     0 ]
//      [        0          0  2*mu ]
//
//
//    The user should provide 2D triangular mesh
//    (preferrably, a .vtk file which can be generated by Gmsh for example)
//    which is built for (0;1)x(0;1)
//
//    The code will then
//    - process mesh,
//    - init tags,
//    - assemble linear system,
//    - solve it with INMOST inner linear solver,
//    - save solution in a .vtk file.


using namespace INMOST;
using namespace std;

enum{
    T_ASSEMBLE = 0,
    T_SOLVE,
    T_PRECOND,
    T_IO,
    T_INIT,
    T_UPDATE
};

const string tagNameTensor = "ELASTIC_TENSOR";
const string tagNameBC     = "BOUNDARY_CONDITION";
const string tagNameRHS    = "RHS";
const string tagNameSol    = "Displacement";
const string tagNameSolEx  = "Displacement_Analytical";
const string tagNameStress = "Stress";

const double M_PI = 3.1415926535898;
const double E    = 3.5e6;                   // Young's modulus
const double nu      = 0.3;                  // Poisson ratio
const double lam     = E*nu/(1+nu)/(1-2*nu);
const double mu      = E/2/(1+nu);

void exactSolution(double *x, double *res)
{
    res[0] = 0.0;
    res[1] = 0.0;
}

void exactSolutionRHS(double *x, double *res)
{
    res[0] = -3e7;
    res[1] = 0.0;
}

class Problem
{
private:
    Mesh m;
    // List of mesh tags
    Tag tagC;      // Elastic tensor
    Tag tagBC;     // Boundary conditions
    Tag tagSol;    // Solution (displacement)
    Tag tagSolEx;  // Exact solution
    Tag tagRHS;    // RHS function f
    Tag tagStress; // Stress tensor

    MarkerType mrkDirNode;  // Dirichlet node marker
    MarkerType mrkUnknwn;   // Node with unknown

    Automatizator aut;       // Automatizator to handle all AD things
    Residual R;              // Residual to assemble
    dynamic_variable Ux, Uy; // X,Y displacements

    unsigned numDirNodes;

    double times[10];
    double ttt; // global timer

public:
    Problem(string meshName);
    ~Problem();
    void initProblem(); // create tags and set parameters
    void assembleGlobalSystem(); // assemble global linear system
    void assembleLocalSystem(Cell &, rMatrix &, rMatrix &);
    void solveSystem();
    void saveSolution(string path); // save mesh with solution
};

Problem::Problem(string meshName)
{
    ttt = Timer();
    for(int i = 0; i < 10; i++)
        times[i] = 0.;

    double t = Timer();
    m.Load(meshName);
    cout << "Number of cells: " << m.NumberOfCells() << endl;
    cout << "Number of faces: " << m.NumberOfFaces() << endl;
    cout << "Number of edges: " << m.NumberOfEdges() << endl;
    cout << "Number of nodes: " << m.NumberOfNodes() << endl;
    m.AssignGlobalID(NODE);
    times[T_IO] += Timer() - t;
}

Problem::~Problem()
{
    printf("\n+=========================\n");
    printf("| T_assemble = %lf\n", times[T_ASSEMBLE]);
    printf("| T_precond  = %lf\n", times[T_PRECOND]);
    printf("| T_solve    = %lf\n", times[T_SOLVE]);
    printf("| T_IO       = %lf\n", times[T_IO]);
    printf("| T_update   = %lf\n", times[T_UPDATE]);
    printf("| T_init     = %lf\n", times[T_INIT]);
    printf("+-------------------------\n");
    printf("| T_total    = %lf\n", Timer() - ttt);
    printf("+=========================\n");
}

void Problem::initProblem()
{
    double t = Timer();
    tagC      = m.CreateTag(tagNameTensor, DATA_REAL, CELL, NONE, 9);
    tagBC     = m.CreateTag(tagNameBC,     DATA_REAL, NODE, NODE, 2);
    tagSol    = m.CreateTag(tagNameSol,    DATA_REAL, NODE, NONE, 3);
    tagSolEx  = m.CreateTag(tagNameSolEx,  DATA_REAL, NODE, NONE, 2);
    tagRHS    = m.CreateTag(tagNameRHS,    DATA_REAL, NODE, NONE, 2);
    tagStress = m.CreateTag(tagNameStress, DATA_REAL, NODE, NONE, 3);

    // Set elastic tensor,
    // also check that all cells are triangles
    for(auto icell = m.BeginCell(); icell != m.EndCell(); icell++){
        if(icell->GetStatus() == Element::Ghost)
            continue;
        if(icell->getNodes().size() != 3){
            cout << "Non-triangular cell" << endl;
            exit(1);
        }
        icell->RealArray(tagC)[0] = 2.*mu + lam;
        icell->RealArray(tagC)[1] = lam;
        icell->RealArray(tagC)[2] = 0.;
        icell->RealArray(tagC)[3] = lam;
        icell->RealArray(tagC)[4] = 2.*mu + lam;
        icell->RealArray(tagC)[5] = 0.;
        icell->RealArray(tagC)[6] = 0.;
        icell->RealArray(tagC)[7] = 0.;
        icell->RealArray(tagC)[8] = 2.*mu;
    }
    m.ExchangeData(tagC, CELL);

    mrkUnknwn = m.CreateMarker();
    mrkDirNode = m.CreateMarker();

    // Set boundary conditions
    // Mark and count Dirichlet nodes
    // Compute RHS and exact solution
    numDirNodes = 0;
    mrkDirNode = m.CreateMarker();
    for(auto inode = m.BeginNode(); inode != m.EndNode(); inode++){
        if(inode->GetStatus() == Element::Ghost)
            continue;
        Node node = inode->getAsNode();
        double x[2], exU[2], exRHS[2];
        node.Barycenter(x);
        exactSolution(x, exU);
        exactSolutionRHS(x, exRHS);

        node.RealArray(tagRHS)[0] = exRHS[0];
        node.RealArray(tagRHS)[1] = exRHS[1];
        node.RealArray(tagSolEx)[0] = exU[0];
        node.RealArray(tagSolEx)[1] = exU[1];

        if(!node.Boundary()){
            node->SetMarker(mrkUnknwn);
            continue;
        }

        node.SetMarker(mrkDirNode);
        numDirNodes++;
        node.RealArray(tagBC)[0]  = exU[0];
        node.RealArray(tagBC)[1]  = exU[1];
        node.RealArray(tagSol)[0]  = exU[0];
        node.RealArray(tagSol)[1]  = exU[1];
    }
    cout << "Number of Dirichlet nodes: " << numDirNodes << endl;



    Automatizator::MakeCurrent(&aut);

    INMOST_DATA_ENUM_TYPE SolTagEntryIndex = 0;
    SolTagEntryIndex = aut.RegisterTag(tagSol, NODE, mrkUnknwn);
    Ux = dynamic_variable(aut, SolTagEntryIndex, 0);
    Uy = dynamic_variable(aut, SolTagEntryIndex, 1);
    aut.EnumerateEntries();
    R = Residual("fem_elasticity", aut.GetFirstIndex(), aut.GetLastIndex());

    times[T_INIT] += Timer() - t;
    m.Save("init.vtk");
}

void Problem::assembleGlobalSystem()
{
    double t = Timer();
    R.Clear();
    for(auto icell = m.BeginCell(); icell != m.EndCell(); icell++){
        if(icell->GetStatus() == Element::Ghost)
            continue;
        Cell cell = icell->getAsCell();

        //printf("cell %d, vol = %e\n", cell.LocalID(), cell.Volume());

        ElementArray<Node> nodes = icell->getNodes();
        rMatrix W(6,6);
        rMatrix rhs(6,1);
        assembleLocalSystem(cell, W, rhs);

        if(!W.isSymmetric()){
            printf("Nonsymm W\n");
            exit(1);
        }

        if(nodes[0].GetMarker(mrkDirNode)){
            // There's no row corresponding to nodes[0]

            // Displacements in boundary node nodes[0]
            double bcValX = nodes[0].RealArray(tagBC)[0];
            double bcValY = nodes[0].RealArray(tagBC)[1];
            // If nodes[1] is not Dirichlet node,
            // add corresponding part to its equations
            if(!nodes[1].GetMarker(mrkDirNode)){
                R[Ux.Index(nodes[1])] += bcValX * W(2,0);
                R[Ux.Index(nodes[1])] += bcValY * W(2,1);
                R[Uy.Index(nodes[1])] += bcValX * W(3,0);
                R[Uy.Index(nodes[1])] += bcValY * W(3,1);
            }
            if(!nodes[2].GetMarker(mrkDirNode)){
                R[Ux.Index(nodes[2])] += bcValX * W(4,0);
                R[Ux.Index(nodes[2])] += bcValY * W(4,1);
                R[Uy.Index(nodes[2])] += bcValX * W(5,0);
                R[Uy.Index(nodes[2])] += bcValY * W(5,1);
            }
        }
        else{
            R[Ux.Index(nodes[0])] += W(0,0)*Ux(nodes[0]);
            R[Ux.Index(nodes[0])] += W(0,1)*Uy(nodes[0]);
            R[Ux.Index(nodes[0])] += W(0,2)*Ux(nodes[1]);
            R[Ux.Index(nodes[0])] += W(0,3)*Uy(nodes[1]);
            R[Ux.Index(nodes[0])] += W(0,4)*Ux(nodes[2]);
            R[Ux.Index(nodes[0])] += W(0,5)*Uy(nodes[2]);
            R[Uy.Index(nodes[0])] += W(1,0)*Ux(nodes[0]);
            R[Uy.Index(nodes[0])] += W(1,1)*Uy(nodes[0]);
            R[Uy.Index(nodes[0])] += W(1,2)*Ux(nodes[1]);
            R[Uy.Index(nodes[0])] += W(1,3)*Uy(nodes[1]);
            R[Uy.Index(nodes[0])] += W(1,4)*Ux(nodes[2]);
            R[Uy.Index(nodes[0])] += W(1,5)*Uy(nodes[2]);

            R[Ux.Index(nodes[0])] -= rhs(0,0);
            R[Uy.Index(nodes[0])] -= rhs(1,0);

//            R[Ux.Index(nodes[0])] += W(0,0)*Ux(nodes[0]);
//            R[Ux.Index(nodes[0])] += W(0,1)*Ux(nodes[1]);
//            R[Ux.Index(nodes[0])] += W(0,2)*Ux(nodes[2]);
//            R[Ux.Index(nodes[0])] += W(0,3)*Uy(nodes[0]);
//            R[Ux.Index(nodes[0])] += W(0,4)*Uy(nodes[1]);
//            R[Ux.Index(nodes[0])] += W(0,5)*Uy(nodes[2]);
//            R[Uy.Index(nodes[0])] += W(1,0)*Ux(nodes[0]);
//            R[Uy.Index(nodes[0])] += W(1,1)*Ux(nodes[0]);
//            R[Uy.Index(nodes[0])] += W(1,2)*Ux(nodes[1]);
//            R[Uy.Index(nodes[0])] += W(1,3)*Uy(nodes[0]);
//            R[Uy.Index(nodes[0])] += W(1,4)*Uy(nodes[1]);
//            R[Uy.Index(nodes[0])] += W(1,5)*Uy(nodes[2]);
            //R[var.Index(nodes[0])] -= bRHS(0,0);
        }

        if(nodes[1].GetMarker(mrkDirNode)){
            // Dirichlet node
            double bcValX = nodes[1].RealArray(tagBC)[0];
            double bcValY = nodes[1].RealArray(tagBC)[1];
            if(!nodes[0].GetMarker(mrkDirNode)){
                R[Ux.Index(nodes[0])] += bcValX * W(0,2);
                R[Ux.Index(nodes[0])] += bcValY * W(0,3);
                R[Uy.Index(nodes[0])] += bcValX * W(1,2);
                R[Uy.Index(nodes[0])] += bcValY * W(1,3);
            }
            if(!nodes[2].GetMarker(mrkDirNode)){
                R[Ux.Index(nodes[2])] += bcValX * W(4,2);
                R[Ux.Index(nodes[2])] += bcValY * W(4,3);
                R[Uy.Index(nodes[2])] += bcValX * W(5,2);
                R[Uy.Index(nodes[2])] += bcValY * W(5,3);
            }
        }
        else{
            R[Ux.Index(nodes[1])] += W(2,0)*Ux(nodes[0]);
            R[Ux.Index(nodes[1])] += W(2,1)*Uy(nodes[0]);
            R[Ux.Index(nodes[1])] += W(2,2)*Ux(nodes[1]);
            R[Ux.Index(nodes[1])] += W(2,3)*Uy(nodes[1]);
            R[Ux.Index(nodes[1])] += W(2,4)*Ux(nodes[2]);
            R[Ux.Index(nodes[1])] += W(2,5)*Uy(nodes[2]);
            R[Uy.Index(nodes[1])] += W(3,0)*Ux(nodes[0]);
            R[Uy.Index(nodes[1])] += W(3,1)*Uy(nodes[0]);
            R[Uy.Index(nodes[1])] += W(3,2)*Ux(nodes[1]);
            R[Uy.Index(nodes[1])] += W(3,3)*Uy(nodes[1]);
            R[Uy.Index(nodes[1])] += W(3,4)*Ux(nodes[2]);
            R[Uy.Index(nodes[1])] += W(3,5)*Uy(nodes[2]);

            R[Ux.Index(nodes[1])] -= rhs(2,0);
            R[Uy.Index(nodes[1])] -= rhs(3,0);

//            R[Ux.Index(nodes[1])] += W(2,0)*Ux(nodes[0]);
//            R[Ux.Index(nodes[1])] += W(2,1)*Ux(nodes[1]);
//            R[Ux.Index(nodes[1])] += W(2,2)*Ux(nodes[2]);
//            R[Ux.Index(nodes[1])] += W(2,3)*Uy(nodes[0]);
//            R[Ux.Index(nodes[1])] += W(2,4)*Uy(nodes[1]);
//            R[Ux.Index(nodes[1])] += W(2,5)*Uy(nodes[2]);
//            R[Uy.Index(nodes[1])] += W(3,0)*Ux(nodes[0]);
//            R[Uy.Index(nodes[1])] += W(3,1)*Ux(nodes[1]);
//            R[Uy.Index(nodes[1])] += W(3,2)*Ux(nodes[2]);
//            R[Uy.Index(nodes[1])] += W(3,3)*Uy(nodes[0]);
//            R[Uy.Index(nodes[1])] += W(3,4)*Uy(nodes[1]);
//            R[Uy.Index(nodes[1])] += W(3,5)*Uy(nodes[2]);
        }

        if(nodes[2].GetMarker(mrkDirNode)){
            // Dirichlet node
            double bcValX = nodes[2].RealArray(tagBC)[0];
            double bcValY = nodes[2].RealArray(tagBC)[1];
            if(!nodes[1].GetMarker(mrkDirNode)){
                R[Ux.Index(nodes[1])] += bcValX * W(2,4);
                R[Ux.Index(nodes[1])] += bcValY * W(2,5);
                R[Uy.Index(nodes[1])] += bcValX * W(3,4);
                R[Uy.Index(nodes[1])] += bcValY * W(3,5);
            }
            if(!nodes[0].GetMarker(mrkDirNode)){
                R[Ux.Index(nodes[0])] += bcValX * W(0,4);
                R[Ux.Index(nodes[0])] += bcValY * W(0,5);
                R[Uy.Index(nodes[0])] += bcValX * W(1,4);
                R[Uy.Index(nodes[0])] += bcValY * W(1,5);
            }
        }
        else{
            R[Ux.Index(nodes[2])] += W(4,0)*Ux(nodes[0]);
            R[Ux.Index(nodes[2])] += W(4,1)*Uy(nodes[0]);
            R[Ux.Index(nodes[2])] += W(4,2)*Ux(nodes[1]);
            R[Ux.Index(nodes[2])] += W(4,3)*Uy(nodes[1]);
            R[Ux.Index(nodes[2])] += W(4,4)*Ux(nodes[2]);
            R[Ux.Index(nodes[2])] += W(4,5)*Uy(nodes[2]);
            R[Uy.Index(nodes[2])] += W(5,0)*Ux(nodes[0]);
            R[Uy.Index(nodes[2])] += W(5,1)*Uy(nodes[0]);
            R[Uy.Index(nodes[2])] += W(5,2)*Ux(nodes[1]);
            R[Uy.Index(nodes[2])] += W(5,3)*Uy(nodes[1]);
            R[Uy.Index(nodes[2])] += W(5,4)*Ux(nodes[2]);
            R[Uy.Index(nodes[2])] += W(5,5)*Uy(nodes[2]);

            R[Ux.Index(nodes[2])] -= rhs(4,0);
            R[Uy.Index(nodes[2])] -= rhs(5,0);

//            R[Ux.Index(nodes[2])] += W(4,0)*Ux(nodes[0]);
//            R[Ux.Index(nodes[2])] += W(4,1)*Ux(nodes[1]);
//            R[Ux.Index(nodes[2])] += W(4,2)*Ux(nodes[2]);
//            R[Ux.Index(nodes[2])] += W(4,3)*Uy(nodes[0]);
//            R[Ux.Index(nodes[2])] += W(4,4)*Uy(nodes[1]);
//            R[Ux.Index(nodes[2])] += W(4,5)*Uy(nodes[2]);
//            R[Uy.Index(nodes[2])] += W(5,0)*Ux(nodes[0]);
//            R[Uy.Index(nodes[2])] += W(5,1)*Ux(nodes[1]);
//            R[Uy.Index(nodes[2])] += W(5,2)*Ux(nodes[2]);
//            R[Uy.Index(nodes[2])] += W(5,3)*Uy(nodes[0]);
//            R[Uy.Index(nodes[2])] += W(5,4)*Uy(nodes[1]);
//            R[Uy.Index(nodes[2])] += W(5,5)*Uy(nodes[2]);
        }
    }
    times[T_ASSEMBLE] += Timer() - t;
}

void Problem::assembleLocalSystem(Cell &cell, rMatrix &W, rMatrix &rhs)
{
    ElementArray<Node> nodes = cell.getNodes();

    double x0[2], x1[2], x2[2];
    nodes[0].Barycenter(x0);
    nodes[1].Barycenter(x1);
    nodes[2].Barycenter(x2);

    rMatrix Ck(3,3); // Stiffness tensor
    Ck.Zero();
    for(unsigned i = 0; i < 3; i++)
        for(unsigned j = 0; j < 3; j++)
            Ck(i,j) = cell.RealArray(tagC)[i*3+j];

    rMatrix R(3,6), PhiGrad(3,2);
//    PhiGrad(0,0) = x1[1] - x2[1]; // y2 - y3
//    PhiGrad(0,1) = x2[0] - x1[0]; // x3 - x2
//    PhiGrad(1,0) = x2[1] - x0[1]; // y3 - y1
//    PhiGrad(1,1) = x0[0] - x2[0]; // x1 - x3
//    PhiGrad(2,0) = x0[1] - x1[1]; // y1 - y2
//    PhiGrad(2,1) = x2[0] - x0[0]; // x3 - x1
//    PhiGrad *= 0.5/cell.Volume();

    rMatrix A(3,3), B(3,2);
    A(0,0) = 1.;
    A(0,1) = 1.;
    A(0,2) = 1.;
    A(1,0) = x0[0];
    A(1,1) = x1[0];
    A(1,2) = x2[0];
    A(2,0) = x0[1];
    A(2,1) = x1[1];
    A(2,2) = x2[1];

    B.Zero();
    B(1,0) = 1.;
    B(2,1) = 1.;

    PhiGrad = A.Invert() * B;

    R.Zero();
    R(0,0) = PhiGrad(0,0);
    R(0,2) = PhiGrad(0,1);
    R(0,4) = PhiGrad(1,0);
    R(2,0) = PhiGrad(1,1);
    R(2,2) = PhiGrad(2,0);
    R(2,4) = PhiGrad(2,1);

    R(2,1) = PhiGrad(0,0);
    R(2,3) = PhiGrad(0,1);
    R(2,5) = PhiGrad(1,0);
    R(1,1) = PhiGrad(1,1);
    R(1,3) = PhiGrad(2,0);
    R(1,5) = PhiGrad(2,1);

//    R.Print();
//    exit(1);

    double detA = A(0,0)*A(1,1)*A(2,2) + A(0,1)*A(1,2)*A(2,0) + A(0,2)*A(1,0)*A(2,1);
    detA       -= A(0,2)*A(1,1)*A(2,0) + A(2,1)*A(1,2)*A(0,0) + A(2,2)*A(1,0)*A(0,1);


    W = detA * 0.5 * R.Transpose() * Ck * R;

    // rhs assembly
    rMatrix Bk(2,2);
    Bk(0,0) = x1[0] - x0[0]; //x2 - x1;
    Bk(0,1) = x2[0] - x0[0]; //x3 - x1;
    Bk(1,0) = x1[1] - x0[1]; //y2 - y1;
    Bk(1,1) = x2[1] - x0[1]; //y3 - y1;

    double detBk = Bk(0,0)*Bk(1,1) - Bk(0,1)*Bk(1,0);
    rhs(0,0) = nodes[0].RealArray(tagRHS)[0] + nodes[1].RealArray(tagRHS)[0] + nodes[2].RealArray(tagRHS)[0];
    rhs(1,0) = nodes[0].RealArray(tagRHS)[1] + nodes[1].RealArray(tagRHS)[1] + nodes[2].RealArray(tagRHS)[1];
    rhs(2,0) = rhs(0,0);
    rhs(3,0) = rhs(1,0);
    rhs(4,0) = rhs(0,0);
    rhs(5,0) = rhs(1,0);
    rhs *= fabs(detBk) / 18.;
}

rMatrix integrateRHS(Cell &cell)
{
    rMatrix res(3,1);

    ElementArray<Node> nodes = cell.getNodes();

    double x0[2], x1[2], x2[2];
    nodes[0].Barycenter(x0);
    nodes[1].Barycenter(x1);
    nodes[2].Barycenter(x2);

    rMatrix Bk(2,2);
    Bk(0,0) = x1[0] - x0[0]; //x2 - x1;
    Bk(0,1) = x2[0] - x0[0]; //x3 - x1;
    Bk(1,0) = x1[1] - x0[1]; //y2 - y1;
    Bk(1,1) = x2[1] - x0[1]; //y3 - y1;

    rMatrix Ck = Bk.Invert() * Bk.Invert().Transpose();

    double detBk = Bk(0,0)*Bk(1,1) - Bk(0,1)*Bk(1,0);

    res.Zero();
//    res(0,0) += exactSolutionRHS(x0) + exactSolutionRHS(x1) + exactSolutionRHS(x2);
//    res(1,0) = res(0,0);
//    res(2,0) = res(0,0);

    return res * fabs(detBk) / 18.;
}

void Problem::solveSystem()
{
    Solver S("inner_mptiluc");
    S.SetParameter("relative_tolerance", "1e-12");
    S.SetParameter("absolute_tolerance", "1e-15");
    double t = Timer();
    S.SetMatrix(R.GetJacobian());
    times[T_PRECOND] += Timer() - t;
    Sparse::Vector sol;
    sol.SetInterval(aut.GetFirstIndex(), aut.GetLastIndex());

//    Sparse::Matrix &A = R.GetJacobian();
//    unsigned N = sol.Size();
//    rMatrix M(N,N);
//    for(unsigned i = 0; i < N; i++)
//        for(unsigned j = 0; j < N; j++)
//            M(i,j) = A[i][j];

//    if(!M.isSymmetric()){
//        M.Print();
//        printf("Global matrix is not symmetric\n");
//        exit(1);
//    }


    for(unsigned i = 0; i < sol.Size(); i++){
        sol[i] = i;
        //cout << "b["<<i<<"] = " << R.GetResidual()[i] << endl;
    }
    t = Timer();
    bool solved = S.Solve(R.GetResidual(), sol);
    times[T_SOLVE] += Timer() - t;
    if(!solved){
        cout << "Linear solver failed: " << S.GetReason() << endl;
        cout << "Residual: " << S.Residual() << endl;
        exit(1);
    }
    cout << "Linear solver iterations: " << S.Iterations() << endl;


//    for(unsigned i = 0; i < sol.Size(); i++){
//        if(fabs(sol[i]) > 1e-10){
//            printf("Bad sol[%d] = %e\n", i, sol[i]);
//        }
//    }

    t = Timer();
    double Cnorm = 0.0;
    for(auto inode = m.BeginNode(); inode != m.EndNode(); inode++){
        if(inode->GetMarker(mrkDirNode))
            continue;

        inode->RealArray(tagSol)[0] -= sol[Ux.Index(inode->getAsNode())];
        inode->RealArray(tagSol)[1] -= sol[Uy.Index(inode->getAsNode())];
        Cnorm = max(Cnorm, fabs(inode->RealArray(tagSol)[0]-inode->RealArray(tagSolEx)[0]));
        Cnorm = max(Cnorm, fabs(inode->RealArray(tagSol)[1]-inode->RealArray(tagSolEx)[1]));
    }
    cout << "|err|_C = " << Cnorm << endl;
    times[T_UPDATE] += Timer() - t;
}

void Problem::saveSolution(string path)
{
    double t = Timer();
    m.Save(path);

    for(auto inode = m.BeginNode(); inode != m.EndNode(); inode++){
        auto coords = inode->Coords();
        coords[0] += inode->RealArray(tagSol)[0];
        coords[1] += inode->RealArray(tagSol)[1];
    }

    m.Save("deformed.vtk");

    times[T_IO] += Timer() - t;
}


int main(int argc, char *argv[])
{
    if(argc != 2){
        cout << "Usage: 2d_elasticity_fem <mesh_file>" << endl;
        return 1;
    }

    Problem P(argv[1]);
    P.initProblem();
    P.assembleGlobalSystem();
    P.solveSystem();
    P.saveSolution("res.vtk");

    return 0;
}
