#include "TrajectoryFollower.hpp"
#include <base/Logging.hpp>

using namespace Eigen;
using namespace trajectory_follower;
    
double TrajectoryFollower::angleLimit( double angle )
{
    if( angle > M_PI )
	return angle - 2*M_PI;
    else if( angle < -M_PI )
	return angle + 2*M_PI;
    else
     	return angle;
}

TrajectoryFollower::TrajectoryFollower()
    : configured( false ),
    controllerType( CONTROLLER_UNKNOWN )
{
    data.followerStatus = TRAJECTORY_FINISHED;
}

TrajectoryFollower::TrajectoryFollower( const FollowerConfig& followerConfig )
    : configured( false ),
    trajectoryConfig( followerConfig.trajectoryConfig ),
    controllerType( followerConfig.controllerType )
{
    poseTransform.setPose( base::Pose( followerConfig.poseTransform ) );

    base::Vector6d rotZ90; 
    rotZ90 << 0, 0, M_PI, 0, 0, 0;
    backTransform.setPose( base::Pose( rotZ90 ) );

    data.followerStatus = TRAJECTORY_FINISHED;

    // Configures the controller according to controller type
    if( controllerType == CONTROLLER_NO_ORIENTATION )
    {
        // No orientation controller
        noOrientationController = NoOrientationController( 
                followerConfig.noOrientationControllerConfig );
    }
    else if( controllerType == CONTROLLER_CHAINED )
    {
        // Chained controller
        chainedController = ChainedController( 
                followerConfig.chainedControllerConfig );
    }
    else
    {
        throw std::runtime_error("Wrong controller type given, it should be  "
                "either CONTROLLER_NO_ORIENTATION (0) or CONTROLLER_CHAINED (1).");
    }

    configured = true;
}

void TrajectoryFollower::setNewTrajectory( const base::Trajectory &trajectory_,
    const base::samples::RigidBodyState& robotPose )
{
    if( !configured )
    {
        throw std::runtime_error("TrajectoryFollower not configured.");
    }

    // Sets the trajectory
    trajectory = trajectory_;

    // Sets the geometric resolution
    trajectory.spline.setGeometricResolution( 
            trajectoryConfig.geometricResolution );

    // Curve parameter and length
    data.curveParameter = trajectory.spline.getStartParam();
    data.curveLength = trajectory.spline.getCurveLength( 
            trajectoryConfig.geometricResolution );

    // Computes the current pose, reference pose and the errors
    computeErrors( robotPose );

    // Initialize based on controller type
    if( controllerType == CONTROLLER_NO_ORIENTATION )
    {
        // Resets the controlelr
        noOrientationController.reset();

        // Checks initial stability of the trajectory 
        if( !noOrientationController.initialStable( data.distanceError, 
                    data.angleError, trajectory.spline.getCurvatureMax() )  ) 
        {
            data.followerStatus = INITIAL_STABILITY_FAILED;
            return;
        }

    }
    else if( controllerType == CONTROLLER_CHAINED )
    {
        // Reset the controller
        chainedController.reset();

        // Checks initial stability of the trajectory 
        if( !chainedController.initialStable( data.distanceError, 
                    data.angleError, 
                    trajectory.spline.getCurvature( data.curveParameter ),
                    trajectory.spline.getCurvatureMax() ) )
        {
            data.followerStatus = INITIAL_STABILITY_FAILED;
            return;
        }
    }

    // Direction
    direction = ( trajectory.driveForward() ? 1 : -1 );

    // Set state as following if stable
    data.followerStatus = TRAJECTORY_FOLLOWING;
}
  
void TrajectoryFollower::computeErrors( const base::samples::RigidBodyState& robotPose )
{
    // Transform robot pose into pose of the center of rotation
    if( trajectory.driveForward() )
    {
        data.currentPose.setTransform( robotPose.getTransform() * 
                poseTransform.getTransform() );
    }
    else
    {
        data.currentPose.setTransform( robotPose.getTransform() * 
                poseTransform.getTransform() *
                backTransform.getTransform() );
    }

    // Gets the heading of the current pose
    data.currentHeading = angleLimit( data.currentPose.getYaw() );

    // No orientation controller
    if( controllerType == CONTROLLER_NO_ORIENTATION )
    {
        // No orientation controller actual point is offset by given value 
        // and based on direction of movement
        data.currentPose.position += AngleAxisd( data.currentHeading, Vector3d::UnitZ() )
            * Vector3d( noOrientationController.getConfig().l1, 0, 0);
    }

    // Find the closest point on the curve and gets the distance error and 
    // heading error at this point 
    //
    // TODO replace this with a spline segment which is in expected limits and 
    // find the reference point w.r.t this 
    Eigen::Vector3d error = trajectory.spline.poseError( data.currentPose.position, 
            data.currentHeading, data.curveParameter );

    data.distanceError  = error(0); // Distance error
    data.angleError     = error(1); // Heading error
    data.curveParameter = error(2); // Curve parameter of reference point

    // Setting reference values
    data.referenceHeading = trajectory.spline.getHeading( data.curveParameter );
    data.referencePose.position = trajectory.spline.getPoint( data.curveParameter ); 	    
    data.referencePose.orientation = AngleAxisd( data.referenceHeading, Vector3d::UnitZ() );
}

FollowerStatus TrajectoryFollower::traverseTrajectory( 
        base::commands::Motion2D &motionCmd, 
        const base::samples::RigidBodyState &robotPose )
{   
    motionCmd.translation = 0;
    motionCmd.rotation = 0;

    // Return if there is no trajectory to follow
    if( data.followerStatus != TRAJECTORY_FOLLOWING )
    {
        LOG_INFO_S << "Trajectory follower not active";
        return data.followerStatus;
    }

    data.time = base::Time::now();

    // Computes reference pose and the errors
    computeErrors( robotPose );

    // Finding reached end condition
    double reachedEnd = false;

    // If distance to trajectory finish set
    if( base::isUnset< double >( trajectoryConfig.trajectoryFinishDistance ) ) 
    {
        // Only curve parameter
        if( ! (data.curveParameter < trajectory.spline.getEndParam()) ) 
        {
            reachedEnd = true;
        }
    }
    else
    {
        data.distanceToEnd = trajectory.spline.getCurveLength( data.curveParameter, 
                    trajectoryConfig.geometricResolution ); 
        // Distance along curve to end point
        if( data.distanceToEnd <= trajectoryConfig.trajectoryFinishDistance  ) 
        {
            reachedEnd = true;
        }

    }

    // If end reached
    if( reachedEnd )
    {
        // Trajectory finished
        LOG_INFO_S << "Trajectory follower finished";
        data.followerStatus = TRAJECTORY_FINISHED;
        return data.followerStatus;
    }

    // If trajectory follower running call the controller based on the 
    // controller type
    if( controllerType == CONTROLLER_NO_ORIENTATION )
    {
        // No orientation controller update
        motionCmd = noOrientationController.update( fabs( trajectory.speed ), 
                data.distanceError, data.angleError ); 
    }
    else if( controllerType == CONTROLLER_CHAINED )
    {
        // Chained controller update
        motionCmd = chainedController.update( fabs( trajectory.speed ), 
                data.distanceError, 
                data.angleError, 
                trajectory.spline.getCurvature( data.curveParameter ),
                trajectory.spline.getVariationOfCurvature( data.curveParameter ));
    }

    motionCmd.translation = motionCmd.translation * direction;
    motionCmd.rotation = motionCmd.rotation * direction;
    
    if( !base::isUnset< double >( trajectoryConfig.maxRotationalVelocity ) )
    {
        ///< Sets limits on rotational velocity
        motionCmd.rotation = std::min( motionCmd.rotation,  trajectoryConfig.maxRotationalVelocity );
        motionCmd.rotation = std::max( motionCmd.rotation, -trajectoryConfig.maxRotationalVelocity );
    }        

    return data.followerStatus;    
}
