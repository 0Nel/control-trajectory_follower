#include "TrajectoryFollower.hpp"
#include <base-logging/Logging.hpp>

#include <limits>

using namespace trajectory_follower;

TrajectoryFollower::TrajectoryFollower()
    : configured(false),
      controllerType(CONTROLLER_UNKNOWN ),
      automaticPointTurn(false),
      pointTurnDirection(1.)
{
    followerStatus = TRAJECTORY_FINISHED;
    nearEnd = false;
    splineReferenceErrorCoefficient = 0.;
}

TrajectoryFollower::TrajectoryFollower(const FollowerConfig& followerConfig)
    : configured(false),
      controllerType(followerConfig.controllerType),
      automaticPointTurn(false),
      pointTurnDirection(1.),
      followerConf(followerConfig)
{
    followerStatus = TRAJECTORY_FINISHED;
    dampingCoefficient = base::unset< double >();

    // Configures the controller according to controller type
    switch (controllerType) {
    case CONTROLLER_NO_ORIENTATION:
        controller.reset(new NoOrientationController(followerConf.noOrientationControllerConfig));
        break;
    case CONTROLLER_CHAINED:
        controller.reset(new ChainedController(followerConf.chainedControllerConfig));
        break;
    case CONTROLLER_SAMSON:
        controller.reset(new SamsonController(followerConf.samsonControllerConfig));
        break;
    default:
        throw std::runtime_error("Wrong or no controller type given.");
        break;
    }

    if (!base::isUnset< double >(followerConf.dampingAngleUpperLimit))
        dampingCoefficient = 1/std::log(base::Angle::fromRad(followerConf.dampingAngleUpperLimit).getDeg()+1.);

    configured = true;
    nearEnd = false;

    splineReferenceErrorCoefficient = 0.;
    if (!base::isUnset<double>(followerConf.splineReferenceErrorMarginCoefficient))
        splineReferenceErrorCoefficient = followerConf.splineReferenceErrorMarginCoefficient;
}

void TrajectoryFollower::setNewTrajectory(const SubTrajectory &trajectory, const base::Pose& robotPose)
{
    if (!configured)
        throw std::runtime_error("TrajectoryFollower not configured.");

    controller->reset();

    // Sets the trajectory
    this->trajectory = trajectory;
    nearEnd = false;

    // Sets the geometric resolution
    this->trajectory.setGeometricResolution(followerConf.geometricResolution);

    // Curve parameter and length
    currentPose = robotPose;
    lastPose = currentPose;
    currentCurveParameter = this->trajectory.getStartParam();
    lastPosError = lastAngleError = distanceError = angleError = 0.;
    posError = lastPosError;

    followerData.currentPose.position = currentPose.position;
    followerData.currentPose.orientation = currentPose.orientation;
    base::Pose2D refPose = this->trajectory.getIntermediatePoint(currentCurveParameter);
    followerData.splineReference.position = Eigen::Vector3d(refPose.position.x(), refPose.position.y(), 0.);
    followerData.splineReference.orientation = Eigen::Quaterniond(Eigen::AngleAxisd(refPose.orientation, Eigen::Vector3d::UnitZ()));
    followerData.splineSegmentStart = followerData.splineReference;
    followerData.splineSegmentEnd = followerData.splineReference;
    followerData.currentTrajectory.clear();
    followerData.currentTrajectory.push_back(this->trajectory);

    followerStatus = TRAJECTORY_FOLLOWING;
}

void TrajectoryFollower::computeErrors(const base::Pose& robotPose)
{
    lastPose = currentPose;
    currentPose = robotPose;

    std::cout << "Current Position: " << currentPose.position.x() << " " <<currentPose.position.y() << std::endl;

    // Gets the heading of the current pose
    double currentHeading = currentPose.getYaw();

    std::cout << "Current Heading: " << currentPose.getYaw() << std::endl;

    // Change heading based on direction of motion
    if(!trajectory.driveForward())
        currentHeading = SubTrajectory::angleLimit(currentHeading + M_PI);

    const Eigen::Vector2d movementVector = currentPose.position.head<2>() - lastPose.position.head<2>();
    const double distanceMoved = movementVector.norm();
    const double movementDirection = atan2(movementVector.y(), movementVector.x());

    // FIXME direction should simply be `trajectory.driveForward() ? 1.0 : -1.0`
    double direction = 1.;
    if (std::abs(SubTrajectory::angleLimit(movementDirection - currentHeading)) > base::Angle::fromDeg(90).getRad())
        direction = -1.;

    double errorMargin = distanceMoved*splineReferenceErrorCoefficient;
    if (!base::isUnset<double>(followerConf.splineReferenceError))
        errorMargin += std::abs(followerConf.splineReferenceError);

    //Find upper and lower bound for local search
    double forwardLength, backwardLength;
    forwardLength = distanceMoved + errorMargin;
    backwardLength = forwardLength;

    if (!base::isUnset<double>(followerConf.maxForwardLenght))
        forwardLength = std::min(forwardLength, followerConf.maxForwardLenght);

    if (!base::isUnset<double>(followerConf.maxBackwardLenght))
        backwardLength = std::min(backwardLength, followerConf.maxBackwardLenght);

    double dist = distanceMoved*direction;
    double splineSegmentStartCurveParam, splineSegmentEndCurveParam, splineSegmentGuessCurveParam;
    splineSegmentStartCurveParam = trajectory.advance(currentCurveParameter, -backwardLength);
    splineSegmentEndCurveParam = trajectory.advance(currentCurveParameter, forwardLength);
    splineSegmentGuessCurveParam = trajectory.advance(currentCurveParameter, dist);

    std::cout <<"Updating spline information for FollowerData" << std::endl;

    base::Pose2D splineStartPoint, splineEndPoint;
    splineStartPoint = trajectory.getIntermediatePoint(splineSegmentStartCurveParam);
    splineEndPoint = trajectory.getIntermediatePoint(splineSegmentEndCurveParam);

    std::cout <<"Calculated spline start and end points" << std::endl;

    std::cout <<"Spline start: " << splineStartPoint.position.x() << " " << splineStartPoint.position.y() << std::endl;
    std::cout <<"Spline end: " << splineEndPoint.position.x() << " " << splineEndPoint.position.y() << std::endl;  

    followerData.splineSegmentStart.position.head<2>() = splineStartPoint.position;
    followerData.splineSegmentEnd.position.head<2>() = splineEndPoint.position;
    followerData.splineSegmentStart.orientation = Eigen::Quaterniond(Eigen::AngleAxisd(splineStartPoint.orientation, Eigen::Vector3d::UnitZ()));
    followerData.splineSegmentEnd.orientation = Eigen::Quaterniond(Eigen::AngleAxisd(splineEndPoint.orientation, Eigen::Vector3d::UnitZ()));
    
    currentCurveParameter = trajectory.posSpline.localClosestPointSearch(currentPose.position, splineSegmentGuessCurveParam, splineSegmentStartCurveParam, splineSegmentEndCurveParam);
    std::cout << "Calculated current curver parameter " << std::endl;
    auto err = trajectory.error(currentPose.position.head<2>(), currentPose.getYaw(), currentCurveParameter);
    std::cout << "Calculated error " << std::endl;
    distanceError = err.first;
    lastAngleError = angleError;
    angleError = err.second;

    std::cout << "Angle Error " << angleError << std::endl;
    std::cout << "Distance Error " << distanceError << std::endl;  
}

FollowerStatus TrajectoryFollower::traverseTrajectory(Motion2D &motionCmd, const base::Pose &robotPose)
{
    motionCmd.translation = 0;
    motionCmd.rotation = 0;
    motionCmd.heading = 0;

    // Return if there is no trajectory to follow
    if(followerStatus == TRAJECTORY_FINISHED) {
        LOG_INFO_S << "Trajectory follower not active";
        return followerStatus;
    }
    std::cout << "Starting to traverse " << std::endl; 

    /*
        Here we need to differentiate whether the DriveMode::ModeTurnOnTheSpot is set by the automatic point turn feature of trajectory follower
        or the DriveMode::ModeTurnOnTheSpot is actually required by the planner as part of the planned trajectory 
    */  

    if (trajectory.driveMode == DriveMode::ModeTurnOnTheSpot && automaticPointTurn == false) 
    {
        double actualHeading = robotPose.getYaw();
        double targetHeading = trajectory.goalPose.orientation;

        if (actualHeading < 0)
            actualHeading = 2*M_PI + actualHeading; 

        double error       = actualHeading - targetHeading;

        Eigen::AngleAxisd currentAxisRot(actualHeading,Eigen::Vector3d::UnitZ());
        Eigen::AngleAxisd targetAxisRot(targetHeading,Eigen::Vector3d::UnitZ());
        Eigen::Vector3d currentRot = currentAxisRot * Eigen::Vector3d::UnitX();
        Eigen::Vector3d desiredRot = targetAxisRot  * Eigen::Vector3d::UnitX();  
        Eigen::Vector3d cross      = currentRot.cross(desiredRot).normalized();

        std::cout << "Current Heading " << actualHeading << std::endl;
        std::cout << "Goal Heading "    << targetHeading << std::endl;
        std::cout << "Error  "          << error     << std::endl;

        followerStatus        = EXEC_TURN_ON_SPOT;

        std::cout << "Cross.z " << cross.z() << std::endl;

        if (cross.z() == -1)
            pointTurnDirection = -1.;
        else
            pointTurnDirection =  1.;

        if ((error < - 0.05 || error > 0.05))
        {
            motionCmd.rotation = pointTurnDirection * followerConf.pointTurnVelocity;
            followerData.cmd = motionCmd.toBaseMotion2D();
            return followerStatus;
        }
        else
        {
            std::cout << "Finished Point-turn" << std::endl;
            pointTurnDirection = 1.;
            followerStatus = TRAJECTORY_FINISHED;
            return followerStatus;
        }
    }

    if (!base::isUnset<double>(followerConf.slamPoseErrorCheckEllipseX) && !base::isUnset<double>(followerConf.slamPoseErrorCheckEllipseY)) {
        double rx = std::min(followerConf.slamPoseErrorCheckEllipseX, 0.6), ry = std::min(followerConf.slamPoseErrorCheckEllipseY, 0.45);
        rx = std::max(rx, 0.01), ry = std::max(ry, 0.01);
        double angle = currentPose.getYaw()+angleError;

        const double slamPoseCheckVal = (Eigen::Rotation2Dd(angle) * (robotPose.position.head<2>() - currentPose.position.head<2>()))
                .cwiseQuotient(Eigen::Vector2d(rx, ry)).squaredNorm();


        if (!(slamPoseCheckVal <= 1.)) {
            if (followerStatus != SLAM_POSE_CHECK_FAILED) {
                std::cout << "SLAM_POSE_CHECK_FAILED! slamPoseCheckVal is " << slamPoseCheckVal << std::endl;
                lastFollowerStatus = followerStatus;
                followerStatus = SLAM_POSE_CHECK_FAILED;
            }

            return followerStatus;
        } else if (followerStatus == SLAM_POSE_CHECK_FAILED) {
            followerStatus = lastFollowerStatus;
        }
    }
    std::cout << "Starting to compute errors " << std::endl;
    computeErrors(robotPose);
    std::cout << "Computed errors " << std::endl;

    followerData.angleError = angleError;
    followerData.distanceError = distanceError;

    std::cout << "Angle error: " << angleError << std::endl;
    std::cout << "Distance error: " << distanceError << std::endl;

    followerData.currentPose.position = currentPose.position;
    followerData.currentPose.orientation = currentPose.orientation;
    base::Pose2D refPose = trajectory.getIntermediatePoint(currentCurveParameter);
    followerData.splineReference.position = Eigen::Vector3d(refPose.position.x(), refPose.position.y(), 0.);
    followerData.splineReference.orientation = Eigen::Quaterniond(Eigen::AngleAxisd(refPose.orientation, Eigen::Vector3d::UnitZ()));
    std::cout << "Finished setting FollowerData" << std::endl;

    double distanceToEnd = trajectory.getDistToGoal(currentCurveParameter);
    lastPosError = posError;
    posError = (robotPose.position.head(2) - trajectory.getGoalPose().position).norm();

    bool reachedEnd = false;
    // If distance to trajectory finish set
    if (base::isUnset<double>(followerConf.trajectoryFinishDistance)) {
        // Only curve parameter
        if (!(currentCurveParameter < trajectory.getEndParam()))
            reachedEnd = true;
    } else {
        // Distance along curve to end point
        if (distanceToEnd <= followerConf.trajectoryFinishDistance)
        {
            if (followerConf.usePoseErrorReachedEndCheck)
                nearEnd = true;
            else
                reachedEnd = true;
        }

        if (posError <= followerConf.trajectoryFinishDistance)
            reachedEnd = true;
    }

    if (nearEnd) {
        if (posError > lastPosError)
            reachedEnd = true;
    }

    // If end reached
    if (reachedEnd) {
        // Trajectory finished
        nearEnd = false;
        LOG_INFO_S << "Trajectory follower finished";
        followerStatus = TRAJECTORY_FINISHED;
        return followerStatus;
    }

    if (trajectory.driveMode != DriveMode::ModeSideways && checkTurnOnSpot()) {
        if ((angleError < -followerConf.pointTurnEnd
                || angleError > followerConf.pointTurnEnd)
                && std::signbit(lastAngleError) == std::signbit(angleError))
        {
            motionCmd.rotation = pointTurnDirection * followerConf.pointTurnVelocity;
            followerData.cmd = motionCmd.toBaseMotion2D();
            return followerStatus;
        }
        else
        {
            std::cout << "stopped Point-Turn. Switching to normal controller" << std::endl;
            automaticPointTurn = false;
            pointTurnDirection = 1.;
            followerStatus = TRAJECTORY_FOLLOWING;
        }
    }

   // Lateral driving
    if (trajectory.driveMode == DriveMode::ModeSideways) {
        double currentHeading = currentPose.getYaw();
        base::Position2D vecToGoal = trajectory.getGoalPose().position - trajectory.getStartPose().position;
        double goalHeading = atan2(vecToGoal[1], vecToGoal[0]);

        motionCmd.translation = trajectory.getSpeed();
        motionCmd.heading = goalHeading - currentHeading;
        motionCmd.rotation = 0;

        followerData.cmd = motionCmd.toBaseMotion2D();
        return followerStatus;
    } 
    
    motionCmd = controller->update(trajectory.getSpeed(), distanceError, angleError, trajectory.getCurvature(currentCurveParameter),
                                   trajectory.getVariationOfCurvature(currentCurveParameter));

    // HACK: use damping factor to prevent oscillating steering behavior
    if (!base::isUnset<double>(followerConf.dampingAngleUpperLimit) && followerConf.dampingAngleUpperLimit > 0)
    {
        double dampingFactor = std::min(1., std::log(std::abs(base::Angle::fromRad(motionCmd.rotation).getDeg())+1.)*dampingCoefficient);
        motionCmd.rotation *= dampingFactor;
    }

    if(!base::isUnset< double >(followerConf.maxRotationalVelocity))
    {
        ///< Sets limits on rotational velocity
        motionCmd.rotation = std::min(motionCmd.rotation,  followerConf.maxRotationalVelocity);
        motionCmd.rotation = std::max(motionCmd.rotation, -followerConf.maxRotationalVelocity);
    }

    followerData.cmd = motionCmd.toBaseMotion2D();
    return followerStatus;
}

bool TrajectoryFollower::checkTurnOnSpot()
{
    if (automaticPointTurn)
        return true;

    if(!(angleError > -followerConf.pointTurnStart && angleError < followerConf.pointTurnStart))
    {
        std::cout << "robot orientation : OUT OF BOUND ["  << angleError << ", " << followerConf.pointTurnStart << "]. starting point-turn" << std::endl;
        automaticPointTurn = true;
        followerStatus = EXEC_TURN_ON_SPOT;
        this->trajectory.driveMode = ModeTurnOnTheSpot;

        if (angleError > 0)
            pointTurnDirection = -1.;

        lastAngleError = angleError;
        return true;
    }

    return false;
}
