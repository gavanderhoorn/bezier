#include "bezier_library/bezier_library.hpp"

//////////////////// CONSTRUCTORS & DESTRUCTOR ////////////////////
//Default Constructor
Bezier::Bezier() :
        grind_depth_(0.05), effector_diameter_(0.02), covering_(0.50), mesh_normal_vector_(Eigen::Vector3d::Identity()),
        vector_dir_(Eigen::Vector3d::Identity())
{
    this->inputPolyData_ = vtkSmartPointer<vtkPolyData>::New();
    this->defaultPolyData_ = vtkSmartPointer<vtkPolyData>::New();
    this->printSelf();
}

//Initialized Constructor
Bezier::Bezier(std::string filename_inputMesh, std::string filename_defaultMesh,
                 double grind_depth, double effector_diameter, double covering, int extrication_coefficiant, int extrication_frequency) :
        grind_depth_(grind_depth), effector_diameter_(effector_diameter), covering_(covering), extrication_coefficiant_(extrication_coefficiant),
        extrication_frequency_(extrication_frequency), mesh_normal_vector_(Eigen::Vector3d::Identity()), vector_dir_(Eigen::Vector3d::Identity())
{
    this->inputPolyData_ = vtkSmartPointer<vtkPolyData>::New();
    if(!this->loadPLYPolydata(filename_inputMesh, this->inputPolyData_))
        printf("Can't load input mesh");
    this->defaultPolyData_ = vtkSmartPointer<vtkPolyData>::New();
    if(!this->loadPLYPolydata(filename_defaultMesh, this->defaultPolyData_))
        printf("Can't load default mesh");
    this->printSelf();
}

Bezier::~Bezier(){}

//////////////////// SORTING STRUCT : USE TO ORGANIZE STRIPPER /////////////////
///@brief structure used to reorder lines
struct lineOrganizerStruct
{
        // sort struct have to know its containing objects
        Bezier* bezier_object;
        lineOrganizerStruct(Bezier* bezier_object2) : bezier_object(bezier_object2) {};

        // this is our sort function : use dot products to determine line position
        bool operator() (const std::vector<std::pair<Eigen::Vector3d,Eigen::Vector3d> > &line_a, const std::vector<std::pair<Eigen::Vector3d,Eigen::Vector3d > > &line_b)
        {
            Eigen::Vector3d vector_dir = bezier_object->get_vector_direction();
            float dist_a = vector_dir.dot(line_a[0].first);
            float dist_b = vector_dir.dot(line_b[0].first);
            return dist_a < dist_b;
        }
};

//////////////////// PRIVATE FUNCTIONS ////////////////////
void Bezier::printSelf(void){
    std::cout<<"\n***********************************************\nBEZIER PARAMETERS\n  Grind depth (in centimeters) : "<<
            this->grind_depth_*100<<"\n  Effector diameter (in centimeters) : "<<this->effector_diameter_*100<<
            "\n  Covering (in %) : "<<this->covering_*100<<"/100\n***********************************************"<<std::endl;
}

Eigen::Vector3d Bezier::get_vector_direction(){
    return this->vector_dir_;
}

bool Bezier::loadPLYPolydata(std::string filename, vtkSmartPointer<vtkPolyData> &poly_data)
{
    vtkSmartPointer<vtkPLYReader> reader = vtkSmartPointer<vtkPLYReader>::New(); //reader for ply file
    reader->SetFileName(filename.c_str());
    reader->Update();
    poly_data = reader->GetOutput();
    return true;
}

bool Bezier::savePLYPolyData(std::string filename, vtkSmartPointer<vtkPolyData> poly_data)
{
    vtkSmartPointer<vtkPLYWriter> plyWriter = vtkSmartPointer<vtkPLYWriter>::New();
    plyWriter->SetFileName(filename.c_str());
    plyWriter->SetInputData(poly_data);
    plyWriter->Update();
    if (!plyWriter->Write())
        return false;
    return true;
}

/** fixme dilation problem detected : when depth is to high, dilated mesh has unexpected holes
 * These holes are problematic. In fact, when cutting process is called on dilated mesh, slices are divided in some parts due to these holes
 * and this affects the path generation, especially for extrical trajectory. We have to find a solution, perhaps find best parameters
 * in order to resolve this problem.
 */
bool Bezier::dilatation(double depth, vtkSmartPointer<vtkPolyData> poly_data, vtkSmartPointer<vtkPolyData> &dilate_poly_data)
{
    // Get maximum length of the sides
    double bounds[6];
    this->inputPolyData_->GetBounds(bounds);
    double max_side_length = std::max(bounds[1] - bounds[0], bounds[3] - bounds[2]);
    max_side_length = std::max(max_side_length, bounds[5] - bounds[4]);
    double threshold = depth / max_side_length;

    //dilation
    vtkSmartPointer<vtkImplicitModeller> implicitModeller = vtkSmartPointer<vtkImplicitModeller>::New();
    implicitModeller->SetProcessModeToPerVoxel(); //optimize process  -> per voxel and not per cell
    implicitModeller->SetSampleDimensions(50, 50, 50);
#if VTK_MAJOR_VERSION <= 5
    implicitModeller->SetInput(this->inputPolyData_);
#else
    implicitModeller->SetInputData(this->inputPolyData_);
#endif
    implicitModeller->AdjustBoundsOn();
    implicitModeller->SetAdjustDistance(threshold); // Adjust by 10%
    if(2*threshold>1.0)
      implicitModeller->SetMaximumDistance(1.0);
    else
      implicitModeller->SetMaximumDistance(2*threshold); // 2*threshold in order to be sure -> long time but smoothed dilation
    implicitModeller->ComputeModelBounds(this->inputPolyData_);
    implicitModeller->Update();

    vtkSmartPointer<vtkMarchingCubes> surface = vtkSmartPointer<vtkMarchingCubes>::New();
    surface->SetInputConnection(implicitModeller->GetOutputPort());
    surface->ComputeNormalsOn();
    surface->SetValue(0, depth);
    surface->Update();
    dilate_poly_data = surface->GetOutput();

    //resolve under part of dilation (morphological dilation is usually used on volume not on surface. So, we have to adapt result to our process)
    // Build a Kdtree
    vtkSmartPointer<vtkKdTreePointLocator> kDTree = vtkSmartPointer<vtkKdTreePointLocator>::New();
    kDTree->SetDataSet(this->inputPolyData_);
    kDTree->BuildLocator();
    //Build cell and link in dilate_poly_data
    dilate_poly_data->BuildCells();
    dilate_poly_data->BuildLinks();
    // Get normal tab
    vtkFloatArray *PointNormalArray = vtkFloatArray::SafeDownCast(this->inputPolyData_->GetPointData()->GetNormals());
    if (!PointNormalArray)
        return false;
    // For each cell in dilate_polydata
    for (vtkIdType index_cell = 0; index_cell < (dilate_poly_data->GetNumberOfCells()); index_cell++)
    {
        // Get cell
        vtkCell* cell = dilate_poly_data->GetCell(index_cell);
        // Get center of cell
        double pcoords[3] = {0, 0, 0};
        double *weights = new double[dilate_poly_data->GetMaxCellSize()];
        int subId = cell->GetParametricCenter(pcoords);
        double cellCenter[3] = {0, 0, 0};
        cell->EvaluateLocation(subId, pcoords, cellCenter, weights);
        // Get closest point (in input_polydata)
        vtkIdType iD = kDTree->FindClosestPoint(cellCenter);
        double closestPoint[3];
        this->inputPolyData_->GetPoint(iD, closestPoint);
        // Get direction vector
        Eigen::Vector3d direction_vector = Eigen::Vector3d(cellCenter[0] - closestPoint[0],
                                                           cellCenter[1] - closestPoint[1],
                                                           cellCenter[2] - closestPoint[2]);

        // Get closest point normal
        double normal[3];
        PointNormalArray->GetTuple(iD, normal);
        Eigen::Vector3d normal_vector(normal[0], normal[1], normal[2]);
        // Normalize vectors
        direction_vector.normalize();
        normal_vector.normalize();
        // Test in order to save or remove cell
        if (!vtkMath::IsFinite(cellCenter[0]) ||
            !vtkMath::IsFinite(cellCenter[1]) ||
            !vtkMath::IsFinite(cellCenter[2]) || normal_vector.dot(direction_vector) <= 0)
            dilate_poly_data->DeleteCell(index_cell);
    }
    //remove all deleted cells
    dilate_poly_data->RemoveDeletedCells();
    //check dilated_polydata size
    if(dilate_poly_data->GetNumberOfCells()==0)
        return false;
    return true;
}

bool Bezier::defaultIntersectionOptimisation(vtkSmartPointer<vtkPolyData> &poly_data){
        bool intersection_flag = false;
        // Build a Kdtree on default
        vtkSmartPointer<vtkKdTreePointLocator> kDTreeDefault = vtkSmartPointer<vtkKdTreePointLocator>::New();
        kDTreeDefault->SetDataSet(this->defaultPolyData_);
        kDTreeDefault->BuildLocator();
        vtkFloatArray *defaultPointNormalArray = vtkFloatArray::SafeDownCast(this->defaultPolyData_->GetPointData()->GetNormals());
        // For each cell in dilate polydata
        for (vtkIdType index_cell = 0; index_cell < (poly_data->GetNumberOfCells()); index_cell++)
        {
            // Get cell
            vtkCell* cell = poly_data->GetCell(index_cell);
            //get points of cell
            vtkPoints * pts = cell->GetPoints();
            //variable test use to know points position
            bool inside=false;
            for(int index_pt=0; index_pt<pts->GetNumberOfPoints(); index_pt++){
                //get point
                double pt[3];
                pts->GetPoint(index_pt,pt);
                //get closest point (in defautPolyData)
                vtkIdType iD = kDTreeDefault->FindClosestPoint(pt);
                double closestPoint[3];
                this->defaultPolyData_->GetPoint(iD, closestPoint);
                //get direction vector
                Eigen::Vector3d direction_vector = Eigen::Vector3d(closestPoint[0] - pt[0],
                                                                   closestPoint[1] - pt[1],
                                                                   closestPoint[2] - pt[2] );
                //get closest point normal
                double normal[3];
                defaultPointNormalArray->GetTuple(iD, normal);
                Eigen::Vector3d normal_vector(normal[0], normal[1], normal[2]);
                //normalize vectors
                direction_vector.normalize();
                normal_vector.normalize();
                //Test in order to save or remove cell
                if (vtkMath::IsFinite(pt[0]) &&
                    vtkMath::IsFinite(pt[1]) &&
                    vtkMath::IsFinite(pt[2]) && normal_vector.dot(direction_vector) > 0.1) //fixme threshold use for corner (90° angle) : It resolves problem with neighbor and dot product
                {
                    inside = true;
                    intersection_flag = true;
                    break;
                }
            }
            if (!inside)
                poly_data->DeleteCell(index_cell);

        }
        //remove all deleted cells ( outside cells)
        poly_data->RemoveDeletedCells();
        return intersection_flag;
}

bool Bezier::generateCellNormals(vtkSmartPointer<vtkPolyData> &poly_data){
    vtkSmartPointer<vtkPolyDataNormals> normals = vtkSmartPointer<vtkPolyDataNormals>::New();
    normals->SetInputData(poly_data);
    normals->ComputeCellNormalsOn();
    normals->ComputePointNormalsOff();
    normals->ConsistencyOn();
    normals->AutoOrientNormalsOn();
    normals->Update();
    poly_data = normals->GetOutput();
    vtkFloatArray* CellNormalArray = vtkFloatArray::SafeDownCast(poly_data->GetCellData()->GetArray("Normals"));

    if (!CellNormalArray)
        return false;

    return true;
}

bool Bezier::generatePointNormals(vtkSmartPointer<vtkPolyData> &poly_data){
    vtkSmartPointer<vtkPolyDataNormals> normals = vtkSmartPointer<vtkPolyDataNormals>::New();
    normals->SetInputData(poly_data);
    normals->ComputeCellNormalsOff();
    normals->ComputePointNormalsOn();
    normals->Update();
    poly_data = normals->GetOutput();
    vtkFloatArray *PointNormalArray = vtkFloatArray::SafeDownCast(poly_data->GetPointData()->GetNormals());

    if (!PointNormalArray)
        return false;

    return true;
}

void Bezier::ransac(){
    //Get polydata point cloud (PCL)
    pcl::PolygonMesh mesh;
    pcl::VTKUtils::vtk2mesh(this->inputPolyData_, mesh);
    PointCloudT::Ptr input_cloud(new PointCloudT);
    pcl::fromPCLPointCloud2(mesh.cloud, *input_cloud);

    //Get polydata dimensions
    double x_size = this->inputPolyData_->GetBounds()[1] - this->inputPolyData_->GetBounds()[0];
    double y_size = this->inputPolyData_->GetBounds()[3] - this->inputPolyData_->GetBounds()[2];
    double z_size = this->inputPolyData_->GetBounds()[5] - this->inputPolyData_->GetBounds()[4];
    //Apply RANSAC theorem
    pcl::SACSegmentation<PointT> seg;
    pcl::ModelCoefficients model_coefficients;
    seg.setInputCloud(input_cloud);
    seg.setModelType(pcl::SACMODEL_PLANE);
    seg.setMethodType(pcl::SAC_RANSAC);
    model_coefficients.values.resize(3); //Plane
    //@note : Thresold = max(x_size,y_size,z_size) cos plane model has to fit all points of inputPolyData
    double threshold = std::max(x_size, y_size);
    threshold = std::max(threshold, z_size);
    seg.setDistanceThreshold(threshold);
    seg.setMaxIterations(2000); //@note 2000 (high value) has been set in order to get good result
    pcl::PointIndices inliers;
    seg.segment(inliers, model_coefficients);
    //Set Mesh normal vector
    this->mesh_normal_vector_ = Eigen::Vector3d(model_coefficients.values[0],
                                                model_coefficients.values[1],
                                                model_coefficients.values[2]);
}

void Bezier::generateDirection(){
    //find to simple orthogonal vectors to mesh_normal
    Eigen::Vector3d x_vector = Eigen::Vector3d(this->mesh_normal_vector_[2], 0, -this->mesh_normal_vector_[0]);
    Eigen::Vector3d y_vector = Eigen::Vector3d(0, this->mesh_normal_vector_[2], -this->mesh_normal_vector_[1]);
    //by default, we chose x_vector. But another vector in this plan could be choose.
    x_vector.normalize();
    this->vector_dir_ = x_vector;
}

unsigned int Bezier::determineSliceNumberExpected(vtkSmartPointer<vtkPolyData> poly_data, Eigen::Vector3d vector_dir){
    // Init with extreme values
    double min_value = DBL_MAX;
    double max_value = DBL_MIN;
    //Get point cloud from polydata : We have to use points of cells and not point cloud directly.
    //In fact, in dilation process, several cells have been deleted. Yet, none of points has been deleted.
    for (vtkIdType index_cell = 0; index_cell < (poly_data->GetNumberOfCells()); index_cell++)
    {
        // Get cell
        vtkCell* cell = poly_data->GetCell(index_cell);
        //get points of cell
        vtkPoints * pts = cell->GetPoints();
        for (int i = 0; i < pts->GetNumberOfPoints(); i++)
        {
            double p[3];
            pts->GetPoint(i, p);
            Eigen::Vector3d point(p[0], p[1], p[2]);
            double value = point.dot(vector_dir);
            if (value > max_value)
                max_value = value;
            if (value < min_value)
                min_value = value;
        }
    }
    //Virtual effector size = effector size * (1- covering%)
    //Distance we have to cut = max_value - min_value
    double virtual_effector_diameter = this->effector_diameter_ * (1 - this->covering_);
    double distance = max_value - min_value;
    return std::ceil(distance / virtual_effector_diameter);
}

unsigned int Bezier::getRealSliceNumber(vtkSmartPointer<vtkStripper> stripper, Eigen::Vector3d vector_dir){
    vtkIdType numberOfLines = stripper->GetOutput()->GetNumberOfLines();
    vtkPoints *points = stripper->GetOutput()->GetPoints();
    vtkCellArray *cells = stripper->GetOutput()->GetLines();
    vtkIdType *indices;
    vtkIdType numberOfPoints;
    unsigned int lineCount = 0;
    std::vector<double> dot_vector;
    //For all lines
    for (cells->InitTraversal(); cells->GetNextCell(numberOfPoints, indices); lineCount++)
    {
        double point[3];
        int indice = ceil(numberOfPoints / 2); //get a point in line : we have chosen center point
        if (numberOfPoints <= 1) //Yet, if number of points is to small, get 0 as indice.
            indice = 0;
        //get point
        points->GetPoint(indices[indice], point);
        //scalar product between vector_dir and point[0] vector
        Eigen::Vector3d point_vector(point[0], point[1], point[2]);
        double dot = vector_dir.dot(point_vector);
        //For each scalar products, push back to a vector
        dot_vector.push_back(dot);
    }
    if (dot_vector.size() == 0) //if vector size is null return 0
        return 0;
    //else sort vector
    std::sort(dot_vector.begin(), dot_vector.end());
    //Remove duplicated value or too close values
    int index = 0;
    double value = (this->effector_diameter_*(1-this->covering_))/(2*10); //2 to get rayon, 10 to get 10% of virtual rayon as threshold
    while (index < (dot_vector.size() - 1))
    {
        if (std::abs(dot_vector[index] - dot_vector[index + 1]) < value)
        {
            dot_vector.erase(dot_vector.begin() + index + 1);
        }
        //whitout threshold :
        /*if (dot_vector[index] == dot_vector[index + 1])
         dot_vector.erase(dot_vector.begin() + index + 1);*/
        else
            index++;
    }
    //Return size of vector (equal real number of lines)
    return dot_vector.size();
}

bool Bezier::cutMesh(vtkSmartPointer<vtkPolyData> poly_data, Eigen::Vector3d cut_dir, unsigned int line_number_expected, vtkSmartPointer<vtkStripper> &stripper){
    // Get info about polyData : center point & bounds
    double minBound[3];
    minBound[0] = poly_data->GetBounds()[0];
    minBound[1] = poly_data->GetBounds()[2];
    minBound[2] = poly_data->GetBounds()[4];

    double maxBound[3];
    maxBound[0] = poly_data->GetBounds()[1];
    maxBound[1] = poly_data->GetBounds()[3];
    maxBound[2] = poly_data->GetBounds()[5];

    double center[3];
    center[0] = poly_data->GetCenter()[0];
    center[1] = poly_data->GetCenter()[1];
    center[2] = poly_data->GetCenter()[2];

    double distanceMin = sqrt(vtkMath::Distance2BetweenPoints(minBound, center));
    double distanceMax = sqrt(vtkMath::Distance2BetweenPoints(maxBound, center));

    // Direction for cutter
    cut_dir.normalize();

    // Create a plane to cut mesh
    vtkSmartPointer<vtkPlane> plane = vtkSmartPointer<vtkPlane>::New();
    plane->SetOrigin(poly_data->GetCenter());
    plane->SetNormal(cut_dir[0], cut_dir[1], cut_dir[2]);
    // Create a cutter
    vtkSmartPointer<vtkCutter> cutter = vtkSmartPointer<vtkCutter>::New();
    cutter->SetCutFunction(plane);
#if VTK_MAJOR_VERSION <= 5
    cutter->SetInput(poly_data);
#else
    cutter->SetInputData(poly_data);
#endif
    cutter->Update();
    // Check real number of lines : if holes in mesh, number of lines returns by vtk is wrong
    int line_number_real(0);
    int temp(0);
    while (line_number_real < line_number_expected)
    {
        cutter->GenerateValues(line_number_expected + temp, -distanceMin, distanceMax);
        cutter->Update();
        // VTK triangle filter used for stripper
        vtkSmartPointer<vtkTriangleFilter> triangleFilter = vtkSmartPointer<vtkTriangleFilter>::New();
        triangleFilter->SetInputConnection(cutter->GetOutputPort());
        triangleFilter->Update();
        // VTK Stripper used to generate polylines from cutter
        stripper->SetInputConnection(triangleFilter->GetOutputPort());
        stripper->Update();

        line_number_real = this->getRealSliceNumber(stripper, cut_dir);
        if (line_number_real < line_number_expected)
            temp++;
        std::cout << "\nExpected : " << line_number_expected << " returned : "
                << stripper->GetOutput()->GetNumberOfLines() << " calculated : " << line_number_real << std::endl;
    }
    return true;
}

bool Bezier::generateRobotPoses(Eigen::Vector3d point, Eigen::Vector3d point_next, Eigen::Vector3d normal, Eigen::Affine3d &pose){
    Eigen::Vector3d normal_x(Eigen::Vector3d().Identity());
    Eigen::Vector3d normal_y(Eigen::Vector3d().Identity());
    Eigen::Vector3d normal_z= Eigen::Vector3d(normal[0],normal[1],normal[2]);
    normal_x = point_next-point; //next point direction
    if (normal_x == Eigen::Vector3d::Zero())
    {
        PCL_ERROR("X normal = 0, mesh is too dense or duplicate points in the line!\n");
        return false;
    }
    normal_y = normal_z.cross(normal_x); //determined using others normal vectors

    normal_x.normalize();
    normal_y.normalize();
    normal_z.normalize();

    // Check if pose has NAN values
    if(!vtkMath::IsFinite((float)normal_y[0]) || !vtkMath::IsFinite((float)normal_y[1]) || !vtkMath::IsFinite((float)normal_y[2]))
        return false;
    //else generate matrix(pose)
    //->translation
    pose.translation() << point[0],point[1],point[2];
    //->rotation
    pose.linear().col(0) << normal_x[0], normal_x[1], normal_x[2];
    pose.linear().col(1) << normal_y[0], normal_y[1], normal_y[2];
    pose.linear().col(2) << normal_z[0], normal_z[1], normal_z[2];

    return true;
}

bool Bezier::checkOrientation(std::vector<std::vector<std::pair<Eigen::Vector3d, Eigen::Vector3d> > > &lines)
{
    // Get vector reference
    Eigen::Vector3d reference = this->vector_dir_.cross(this->mesh_normal_vector_);
    reference.normalize();

    //Compare orientation of lines with reference
    for (int line_index = 0; line_index < lines.size(); line_index++)
    {
        // Get line orientation
        int point_number = lines[line_index].size();
        Eigen::Vector3d vector_orientation = lines[line_index][point_number - 1].first - lines[line_index][0].first;
        vector_orientation.normalize();
        //Compare (Check orientation)
        if (reference.dot(vector_orientation) < 0) //dot product<0 so, vectors have opposite orientation.
            std::reverse(lines[line_index].begin(), lines[line_index].end()); // change orientation of line
    }
}

void Bezier::removeNearNeighborPoints(std::vector<std::vector<std::pair<Eigen::Vector3d, Eigen::Vector3d> > > &lines){
    //for each points
    for(int index_line=0; index_line<lines.size();index_line++){
        for(int index_point=0; index_point<(lines[index_line].size()-1);index_point++){
            Eigen::Vector3d point_vector = lines[index_line][index_point].first; //get point position
            Eigen::Vector3d next_point_vector = lines[index_line][index_point+1].first; //get next point position
            double point[3] = {point_vector[0],point_vector[1],point_vector[2]};
            double next_point[3] = {next_point_vector[0],next_point_vector[1],next_point_vector[2]};
            if(sqrt(vtkMath::Distance2BetweenPoints(point, next_point))<0.001){ //fixme arbitrary value
                if(index_point<(lines[index_line].size()-2))
                    lines[index_line].erase(lines[index_line].begin()+index_point+1);
                else
                    lines[index_line].erase(lines[index_line].begin()+index_point);
            }
        }
    }
}

bool Bezier::generateStripperOnSurface(vtkSmartPointer<vtkPolyData> PolyData, std::vector<std::vector<std::pair<Eigen::Vector3d, Eigen::Vector3d> > > &lines){
    // Set slice number (With covering)
    unsigned int slice_number_expected = this->determineSliceNumberExpected(PolyData, this->vector_dir_);
    // Cut mesh
    vtkSmartPointer<vtkStripper> stripper = vtkSmartPointer<vtkStripper>::New();
    cutMesh(PolyData, this->vector_dir_, slice_number_expected, stripper); // fil this stripper with cutMesh function
    //// ************************** ///////////
    ///fixme At the moment we use vector to reorganize data from stripper
    ///      In the future, we must change that in order to use stripper only
    //// ************************** ///////////
    // Get data from stripper
    vtkIdType numberOfLines = stripper->GetOutput()->GetNumberOfLines();
    vtkPoints *points = stripper->GetOutput()->GetPoints();
    vtkCellArray *cells = stripper->GetOutput()->GetLines();
    vtkIdType *indices;
    vtkIdType numberOfPoints;
    unsigned int lineCount = 0;
    // Get normal array of stripper
    vtkFloatArray *PointNormalArray = vtkFloatArray::SafeDownCast(stripper->GetOutput()->GetPointData()->GetNormals());
    if (!PointNormalArray)
        return false;
    for (cells->InitTraversal(); cells->GetNextCell(numberOfPoints, indices); lineCount++) //for each line : iterator
    {
        std::vector<std::pair<Eigen::Vector3d, Eigen::Vector3d> > line; // line.first : point position, line.second : point normal
        for (vtkIdType i = 0; i < numberOfPoints; i++)
        {
            //Get points
            double point[3];
            points->GetPoint(indices[i], point);
            Eigen::Vector3d point_vector(point[0],point[1],point[2]);
            //Get Z normal
            double normal[3];
            PointNormalArray->GetTuple(indices[i], normal);
            Eigen::Vector3d normal_vector(normal[0],normal[1],normal[2]);
            if(PolyData==this->inputPolyData_)
                normal_vector *=-1;
            //add to line vector
            line.push_back(std::make_pair(point_vector,normal_vector));
        }
        lines.push_back(line);
    }
    // Sort vector : re order lines
    std::sort(lines.begin(), lines.end(), lineOrganizerStruct(this));
    // Check line orientation
    checkOrientation(lines);
    // Remove too closed points
    removeNearNeighborPoints(lines);
    return true;
}

int Bezier::seekClosestLine(Eigen::Vector3d point_vector, std::vector<std::vector<std::pair<Eigen::Vector3d, Eigen::Vector3d> > > extrication_lines)
{
    int index_of_closest_line(0);
    double distance(DBL_MAX);
    for (size_t index_line = 0; index_line < extrication_lines.size(); index_line++)
    {
      double number_of_point = extrication_lines[index_line].size();
      Eigen::Vector3d end_extrication_point_vector = extrication_lines[index_line][number_of_point-1].first;
      //get distance
      double point[3] = {point_vector[0], point_vector[1], point_vector[2]};
      double extrication_point[3] = {end_extrication_point_vector[0],
                                     end_extrication_point_vector[1],
                                     end_extrication_point_vector[2]};
      if (distance > vtkMath::Distance2BetweenPoints(point, extrication_point))
      {
        distance = vtkMath::Distance2BetweenPoints(point, extrication_point);
        index_of_closest_line = index_line;
      }
    }
    return index_of_closest_line;
}

//fixme combine seekclosestPoint and seekclosestextricationPassPoint in one function
int Bezier::seekClosestPoint(Eigen::Vector3d point_vector, std::vector<std::pair<Eigen::Vector3d, Eigen::Vector3d> > extrication_line){
    int index(0);
    double distance(DBL_MAX);
    for(size_t index_point=0; index_point<extrication_line.size();index_point++){
       Eigen::Vector3d extrication_point_vector = extrication_line[index_point].first;
       double point[3] = {point_vector[0],point_vector[1],point_vector[2]};
       double extrication_point[3] = {extrication_point_vector[0], extrication_point_vector[1], extrication_point_vector[2]};
       if (distance > vtkMath::Distance2BetweenPoints(point, extrication_point)){
               distance = vtkMath::Distance2BetweenPoints(point, extrication_point);
               index = index_point;
       }
    }
    return index;
}

int Bezier::seekClosestExtricationPassPoint(Eigen::Vector3d point_vector, std::vector<Eigen::Affine3d, Eigen::aligned_allocator<Eigen::Affine3d> > extrication_poses){
  int index(0);
  double distance(DBL_MAX);
  for (size_t index_point = 0; index_point < extrication_poses.size(); index_point++)
  {
    Eigen::Vector3d closest_point_vector = extrication_poses[index_point].translation();
    double point[3] = {point_vector[0], point_vector[1], point_vector[2]};
    double closest_point[3] = {closest_point_vector[0], closest_point_vector[1], closest_point_vector[2]};
    if (distance > vtkMath::Distance2BetweenPoints(point, closest_point))
    {
      distance = vtkMath::Distance2BetweenPoints(point, closest_point);
      index = index_point;
    }
  }
  return index;
}

bool Bezier::saveDilatedMeshes (std::string path)
{
    if (dilationPolyDataVector_.empty())
        return false;

    for (size_t i = 0; i < dilationPolyDataVector_.size(); ++i)
    {
        std::string number (boost::lexical_cast<std::string>(i));
        std::string file (path+"/mesh_"+number+".ply");
        if (!savePLYPolyData(file, dilationPolyDataVector_[i]))
            return false;
        ROS_INFO_STREAM(file << " saved successfully");
    }
    return true;
}


//////////////////// PUBLIC FUNCTION ////////////////////

bool Bezier::generateTrajectory(std::vector<Eigen::Affine3d, Eigen::aligned_allocator<Eigen::Affine3d> > &way_points_vector, std::vector<bool> &color_vector, std::vector<int> &index_vector){
    /////////// CLEAR VECTOR ///////////////
    way_points_vector.clear();
    color_vector.clear();
    /////////// GENERATE NORMAL ON INPUTPOLYDATA ///////////////
    this->generatePointNormals(this->inputPolyData_);
    //////////// GENERATE MESH NORMAL ////////////
    this->ransac();
    //////////// FIND CUT DIRECTION FROM MESH NORMAL ////////////
    this->generateDirection();
    //////////// GENERATE PASSE VECTOR : DILATION PROCESS ////////////
    printf("\nPlease wait : dilation in progress");
    this->dilationPolyDataVector_.push_back(this->inputPolyData_);
    bool intersection_flag=true; //flag variable : dilate while dilated mesh intersect default mesh
    double depth = this->grind_depth_; // depth between input mesh and dilated mesh

    while (intersection_flag)
    {
        vtkSmartPointer<vtkPolyData> dilate_polydata = vtkSmartPointer<vtkPolyData>::New();
        bool flag_dilation = dilatation(depth, this->inputPolyData_, dilate_polydata);
        if (flag_dilation && defaultIntersectionOptimisation(dilate_polydata) && dilate_polydata->GetNumberOfCells() > 10) //fixme //Check intersection between new dilated mesh and default
        {
            this->dilationPolyDataVector_.push_back(dilate_polydata); //if intersection, consider dilated mesh as a pass
            printf("\n  -> New pass generated");
        }
        else
            intersection_flag = false; //No intersection : end of dilation
        depth += this->grind_depth_;
    }
    printf("\nDilation process done");
    //////////// REVERSE PASSE VECTOR : GRIND FROM UPPER PASS  ////////////
    std::reverse(this->dilationPolyDataVector_.begin(),this->dilationPolyDataVector_.end());
    //////////// VARIABLES USE TO GENERATE EXTRICATION MESH ////////////
    vtkSmartPointer<vtkPolyData> extrication_poly_data = vtkSmartPointer<vtkPolyData>::New();
    std::vector<std::vector<std::pair<Eigen::Vector3d, Eigen::Vector3d> > > extrication_lines;

    index_vector.push_back(way_points_vector.size()-1); //push back index of last pose in passe
    //////////// START PROCESS : GENERATE TRAJECTORY ////////////
    for(int polydata_index=0; polydata_index<this->dilationPolyDataVector_.size();polydata_index++){ ///FOR EACH POLYDATA : PASSES
      //////////// GENERATE EXTRICATION MESH ////////////
      if (polydata_index%extrication_frequency_== 0){
        //-> dilated_depth = extrication_coefficiant+numberOfPolydataDilated-1-n*frequency)*grind_depth
        double dilated_depth((extrication_coefficiant_+dilationPolyDataVector_.size()-1-polydata_index)*this->grind_depth_);
        dilatation(dilated_depth, this->inputPolyData_, extrication_poly_data);
        //dilatation(this->extrication_coefficiant_*this->grind_depth_, this->dilationPolyDataVector_[polydata_index], extrication_poly_data);
        generateStripperOnSurface(extrication_poly_data, extrication_lines);
      }
      double dist_to_extrication_mesh((this->extrication_coefficiant_+polydata_index)*this->grind_depth_); //distance between dilationPolyDataVector_[index_polydata] and extrication polydata
      //////////// GENERATE TRAJECTORY ON MESH (POLYDATA) ////////////
      std::vector<std::vector<std::pair<Eigen::Vector3d, Eigen::Vector3d> > > lines;
      this->generateStripperOnSurface(this->dilationPolyDataVector_[polydata_index],lines);
      for(int index_line=0; index_line<lines.size();index_line++){ ///FOR EACH LINE

            ///Variable use to store pose : use for extrication
            Eigen::Affine3d start_pose(Eigen::Affine3d::Identity()); //Start line pose
            Eigen::Affine3d end_pose(Eigen::Affine3d::Identity()); //End line pose

            //////////// GENERATE POSES ON A LINE ////////////
            for(int index_point=0; index_point<lines[index_line].size();index_point++){ ///FOR EACH POINT
              //ignore to small line
              if (lines[index_line].size()<2){
                    printf("Line is too small (number_of_points < 2)\n");
                    break;
               }
               //Get points, normal and generate pose
                Eigen::Vector3d point, next_point, normal;
                Eigen::Affine3d pose(Eigen::Affine3d::Identity());
                bool flag_isFinite = true; //check generated pose for NAN value
                if (index_point < (lines[index_line].size() - 1)){
                    point = lines[index_line][index_point].first;
                    next_point = lines[index_line][index_point+1].first;
                    normal = lines[index_line][index_point].second;
                    flag_isFinite = this->generateRobotPoses(point, next_point, normal, pose);
                }
                else{
                    point = lines[index_line][index_point-1].first;
                    next_point = lines[index_line][index_point].first;
                    normal = lines[index_line][index_point-1].second;
                    flag_isFinite = this->generateRobotPoses(point, next_point, normal, pose);
                    pose.translation() << next_point[0],next_point[1],next_point[2];
                }
                if(index_point == 0){ //FIRST POINT IN LINE
                    start_pose = pose; //Save start pose
                    way_points_vector.push_back(pose); //add pose with false color flag (out of line)
                    color_vector.push_back(false);
                }
                if(flag_isFinite){
                    //put robot pose to vector
                    way_points_vector.push_back(pose);
                    color_vector.push_back(true);
                }
                if(index_point == (lines[index_line].size()-1)){ //LAST POINT IN LINE
                    end_pose = pose;
                    way_points_vector.push_back(pose); //add pose with false color flag (out of line)
                    color_vector.push_back(false);
                }
            }
            //////////// END OF LINE : GENERATE EXTRICATION TO NEXT LINE ////////////
            if(index_line==(lines.size()-1)) //no simple extrication for the last line of mesh
                    break;
            Eigen::Vector3d end_point(end_pose.translation()+dist_to_extrication_mesh*end_pose.linear().col(0));
            Eigen::Vector3d dilated_end_point(end_pose.translation()-dist_to_extrication_mesh*end_pose.linear().col(2));
            Eigen::Vector3d dilated_start_point(start_pose.translation()-dist_to_extrication_mesh*start_pose.linear().col(2));
            //seek closest line in extrication lines
            int index_of_closest_line = seekClosestLine(end_point,extrication_lines);
            //seek for dilated_end_point neighbor in extrication line
            int index_of_closest_end_point = seekClosestPoint(dilated_end_point, extrication_lines[index_of_closest_line]);
            //seek for dilated_start_point neighbor in extrication line
            int index_of_closest_start_point = seekClosestPoint(dilated_start_point,extrication_lines[index_of_closest_line]);
            //get vector between these indexes
            std::vector<std::pair<Eigen::Vector3d, Eigen::Vector3d> > extrication_line(
                  extrication_lines[index_of_closest_line].begin()+index_of_closest_start_point, extrication_lines[index_of_closest_line].begin()+index_of_closest_end_point);
            //generate pose on this vector (line)
            Eigen::Affine3d pose(end_pose);
            Eigen::Vector3d point(Eigen::Vector3d::Identity());
            std::vector<Eigen::Affine3d, Eigen::aligned_allocator<Eigen::Affine3d> > extrication_poses;
            for (int index_point = 0; index_point < extrication_line.size(); index_point++)
            {
                    point = extrication_line[index_point].first;
                    pose.translation() << point[0], point[1], point[2];
                    extrication_poses.push_back(pose);
                    color_vector.push_back(false);
            }
            //reverse extrication pose
            std::reverse(extrication_poses.begin(),extrication_poses.end());
            way_points_vector.insert(way_points_vector.end(), extrication_poses.begin(), extrication_poses.end());
        }
        //////////// EXTRICATION FROM LAST LINE TO FIRST ONE ////////////
        Eigen::Vector3d start_point_pass(lines[0][0].first);
        Eigen::Vector3d start_normal_pass(lines[0][0].second);
        Eigen::Vector3d end_point_pass(lines[lines.size()-1][lines[lines.size()-1].size()].first);
        Eigen::Vector3d end_normal_pass(lines[lines.size()-1][lines[lines.size()-1].size()].second);
        //get vector from last point of last line and first point of first line
        Eigen::Vector3d extrication_pass_dir(end_point_pass-start_point_pass);
        extrication_pass_dir.normalize();
        //get his orthogonal vector to use vtkcutter
          //first step : take his projection on the Ransac plan model
        Eigen::Vector3d extrication_cut_dir(extrication_pass_dir - (extrication_pass_dir.dot(this->mesh_normal_vector_)) * this->mesh_normal_vector_);
          //second step : cross product with mesh normal
        extrication_cut_dir = extrication_cut_dir.cross(this->mesh_normal_vector_);
        extrication_cut_dir.normalize();
        //Cut this dilated mesh to determine extrication pass trajectory
        vtkSmartPointer<vtkStripper> extrication_stripper = vtkSmartPointer<vtkStripper>::New();
        cutMesh(extrication_poly_data, extrication_cut_dir, 1, extrication_stripper);
        //get last pose
        Eigen::Affine3d extrication_pose(Eigen::Affine3d::Identity());
        extrication_pose = way_points_vector.back();
        //check stripper orientation
        vtkPoints *points = extrication_stripper->GetOutput()->GetPoints();
        vtkCellArray *cells = extrication_stripper->GetOutput()->GetLines();
        vtkIdType *indices;
        vtkIdType numberOfPoints;
        vtkIdType lastnumberOfPoints(0); //fixme alternative solution used to face hole problems in dilated mesh
        unsigned int lineCount(0);

        std::vector<Eigen::Affine3d, Eigen::aligned_allocator<Eigen::Affine3d> > extrication_poses;
        Eigen::Vector3d orientation = Eigen::Vector3d::Identity();

        for (cells->InitTraversal(); cells->GetNextCell(numberOfPoints, indices); lineCount++){ //in case where more than one slice has been cut
          if(numberOfPoints>lastnumberOfPoints){ //get the max length stripper
            extrication_poses.clear();
            for (vtkIdType i = 0; i < numberOfPoints; i++){
              double point[3];
              points->GetPoint(indices[i], point);
              extrication_pose.translation() << point[0], point[1], point[2];
              extrication_poses.push_back(extrication_pose);
             }
          }
        }
        orientation = extrication_pose.translation() - extrication_poses[0].translation();
        //check orientation
        if (orientation.dot(extrication_pass_dir) > 0)
            std::reverse(extrication_poses.begin(), extrication_poses.end());
        //seek for closest start pass point index
        int index_end_point_pass = seekClosestExtricationPassPoint(end_point_pass-dist_to_extrication_mesh*end_normal_pass, extrication_poses);
        //seek for closest end pass point index
        int index_start_point_pass = seekClosestExtricationPassPoint(start_point_pass-dist_to_extrication_mesh*start_normal_pass, extrication_poses);
        //get indice of close points
        way_points_vector.insert(way_points_vector.end(), extrication_poses.begin()+index_end_point_pass, extrication_poses.begin()+index_start_point_pass);
        for(size_t i=0; i<(index_start_point_pass-index_end_point_pass);i++){
          color_vector.push_back(false);
        }
        index_vector.push_back(way_points_vector.size()-1); //push back index of last pose in passe
    }
    return true;
}

void Bezier::displayNormal(std::vector<Eigen::Affine3d, Eigen::aligned_allocator<Eigen::Affine3d> > way_points_vector, std::vector<bool> points_color_viz, ros::Publisher &normal_publisher)
{
    //check possible error
    if (way_points_vector.size() != points_color_viz.size())
        PCL_ERROR("Error : Trajectory vector and bool vector have differents sizes in displayTrajectory function \n");
    visualization_msgs::MarkerArray markers;
    for (int k = 0; k < way_points_vector.size(); k++)
    {
        if (points_color_viz[k] == true)
        {
            visualization_msgs::Marker marker;
            marker.header.frame_id = "/base_link";                // Set the frame ID = /base ou /base_link
            marker.header.stamp = ros::Time::now();               //set the timestamp (for tf)
            marker.ns = "basic_shapes";   //set the namespace of the marker
            marker.id = k;        //set an unique ID of the marker
            marker.type = visualization_msgs::Marker::ARROW;
            marker.action = visualization_msgs::Marker::ADD; //set the marker action

            // Set the scale of the marker -- 1x1x1 here means 1m on a side
            marker.scale.x = 0.002; //radius
            marker.scale.y = 0.004; //radius
            //marker.scale.z = 0.001;

            double length = 0.015; //length for normal markers
            geometry_msgs::Point start_point;
            geometry_msgs::Point end_point;
            end_point.x = way_points_vector[k].translation()[0];
            end_point.y = way_points_vector[k].translation()[1];
            end_point.z = way_points_vector[k].translation()[2];
            start_point.x = end_point.x - length * way_points_vector[k].linear().col(2)[0];
            start_point.y = end_point.y - length * way_points_vector[k].linear().col(2)[1];
            start_point.z = end_point.z - length * way_points_vector[k].linear().col(2)[2];

            marker.points.push_back(start_point);
            marker.points.push_back(end_point);

            marker.color.r = 0.0f;
            marker.color.g = 1.0f;
            marker.color.b = 0.0f;
            marker.color.a = 0.7;
            //set the lifetime
            marker.lifetime = ros::Duration();
            markers.markers.push_back(marker);
        }
    }
    while (normal_publisher.getNumSubscribers() < 1)
    {
        ROS_WARN_ONCE("Please create a subscriber to the marker");
        sleep(1);
    }
    normal_publisher.publish(markers);
}

void Bezier::displayTrajectory(std::vector<Eigen::Affine3d, Eigen::aligned_allocator<Eigen::Affine3d> > way_points_vector, std::vector<bool> points_color_viz, ros::Publisher &trajectory_publisher)
{
    //check possible error
    if (way_points_vector.size() != points_color_viz.size())
    {
        PCL_ERROR("Error : Trajectory vector and bool vector have differents sizes in displayTrajectory function \n");
    }
    double diffRepz = 0.45;               // z offset between "/base" and "/base_link"
    visualization_msgs::Marker marker;
    marker.header.frame_id = "/base_link";                // Set frame ID
    marker.header.stamp = ros::Time::now();               //set timestamp (for tf)
    marker.ns = "trajectory";             //Set marker namespace
    marker.id = 0;                //set an unique ID of the marker
    marker.type = visualization_msgs::Marker::LINE_STRIP; //Marker type
    marker.action = visualization_msgs::Marker::ADD; //set the marker action
    marker.lifetime = ros::Duration();            //set the lifetime
    marker.scale.x = 0.003; // Set scale of our new marker : Diameter in our case.
    //Set marker orientation. Here, there is no rotation : (v1,v2,v3)=(0,0,0) angle=0
    marker.pose.orientation.x = 0.0;      // v1* sin(angle/2)
    marker.pose.orientation.y = 0.0;      // v2* sin(angle/2)
    marker.pose.orientation.z = 0.0;      // v3* sin(angle/2)
    marker.pose.orientation.w = 1.0;      // cos(angle/2)

    // Set the pose and color of marker from parameter[in]
    geometry_msgs::Point p; //temporary point
    std_msgs::ColorRGBA color; //temporary color
    for (int k = 1; k < way_points_vector.size(); k++)
    {
        //for each index of trajectory vector, store coordinates in a temporary point p;
        p.x = way_points_vector[k].translation()[0];
        p.y = way_points_vector[k].translation()[1];
        p.z = way_points_vector[k].translation()[2];
        //for each index of boolean vector, check membership of point;
        if (points_color_viz[k] == true)
        {
            color.r = 0.0f;
            color.g = 1.0f; //If points is from grindstone path : Color is green
            color.b = 0.0f;
            color.a = 1.0;
        }
        else
        {
            color.r = 1.0f; //If points is from extrication path : Color is red
            color.g = 0.0f;
            color.b = 0.0f;
            color.a = 1.0;
        }
        marker.colors.push_back(color);     //add color c to LINE_STRIP to set color segment
        marker.points.push_back(p);         //add point p to LINE_STRIP
    }
    while (trajectory_publisher.getNumSubscribers() < 1)
    {
        ROS_WARN_ONCE("Please create a subscriber to the marker");
        sleep(1);
    }
    trajectory_publisher.publish(marker);
}

void Bezier::displayMesh(ros::Publisher &mesh_publisher, std::string mesh_path)
{
    // Create a mesh marker from ply files
    visualization_msgs::Marker mesh_marker;
    mesh_marker.header.frame_id = "/base_link";                // Set the frame ID = /base ou /base_link
    mesh_marker.header.stamp = ros::Time::now();               //set the timestamp (for tf)
    mesh_marker.id = 0;        //set an unique ID of the marker
    mesh_marker.type = visualization_msgs::Marker::MESH_RESOURCE;
    mesh_marker.mesh_resource = mesh_path;
    mesh_marker.action = visualization_msgs::Marker::ADD; //set the marker action

    mesh_marker.scale.x = mesh_marker.scale.y = mesh_marker.scale.z = 1;

    mesh_marker.color.r = 0.6f;
    mesh_marker.color.g = 0.6f;
    mesh_marker.color.b = 0.6f;
    mesh_marker.color.a = 1.0;
    //set the lifetime
    mesh_marker.lifetime = ros::Duration();
    //Publish
    while (mesh_publisher.getNumSubscribers() < 1)
    {
        ROS_WARN_ONCE("Please create a subscriber to the marker");
        sleep(1);
    }
    mesh_publisher.publish(mesh_marker);
}

