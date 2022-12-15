#!/usr/bin/env python3
import numpy as np
import rospy
import tf2_ros
import tf.transformations as tf_convert
import geometry_msgs.msg as geo_msgs
import nav_msgs.msg as nav_msgs
import math
import time
import pickle

vehicle_speed = ''
def vehicle_vel_callback(msg):
    vehicle_speed = msg.data

# Function for adding angles bounded to [0, 2*PI[
def angle_add(angle_1, angle_2):
    if(angle_1 + angle_2 >= 2*math.pi):
        return angle_1 + angle_2 - 2*math.pi
    elif(angle_1 + angle_2 < 0):
        return angle_1 + angle_2 + 2*math.pi
    else:
        return angle_1 + angle_2

# Function to transform a coordinate into world coordinates
def transform_coordinates(x_coordinate, y_coordinate, transform: geo_msgs.Transform):
    
    if not isinstance(transform, geo_msgs.Transform):
        raise TypeError
    else:
        # 4x4 transformation matrix from quaternion and translation
        quaternion = np.array([transform.rotation.x, transform.rotation.y, transform.rotation.z, transform.rotation.w])
        transformation = tf_convert.quaternion_matrix(quaternion)
        transformation[0, 3] = transform.translation.x
        transformation[1, 3] = transform.translation.y
        transformation[2, 3] = transform.translation.z

        # Crack coordinate as pose
        point = np.array([[x_coordinate], [y_coordinate], [0], [1]])

        # Transform coordinate
        result = np.dot(transformation, point)
        x = result[0, 0]
        y = result[1, 0]
        return x, y

def vision_pub(data_in):
    rospy.init_node('vision_publisher', anonymous=True)
    point_pub = rospy.Publisher('/points', geo_msgs.PointStamped, queue_size=10)
    transform_pub = rospy.Publisher('/vo', nav_msgs.Odometry, queue_size=50)

    r = rospy.Rate(100)  # 100hz

    # Transform listener for getting transforms from TF
    tf_buffer = tf2_ros.Buffer(rospy.Time(100))
    tf_listener = tf2_ros.TransformListener(tf_buffer)

    # Transform from camera to base (must be the inverse of base_to_camera_transform)
    transform_camera_to_base = geo_msgs.Transform()
    transform_camera_to_base.translation.x = -0.067
    transform_camera_to_base.translation.y = 0.42665
    transform_camera_to_base.translation.z = 0
    transform_camera_to_base.rotation.x = 0
    transform_camera_to_base.rotation.y = 0
    transform_camera_to_base.rotation.z = 0
    transform_camera_to_base.rotation.w = 1

    world_pose_x = 0
    world_pose_y = 0
    world_orientation = 0

    # Scalar from pixels to distances in camera frame
    PIXEL_SIZE = 0.0009712  # In meters
    STANDARD_COVARIANCE = [1.9074e-05, 0, 0, 0, 0, 0,
                           0, 1.9074e-05, 0, 0, 0, 0,
                           0, 0, 1.9074e-05, 0, 0, 0,
                           0, 0, 0, 1.9074e-05, 0, 0,
                           0, 0, 0, 0, 1.9074e-05, 0,
                           0, 0, 0, 0, 0, 1.9074e-05]

    print('Started vision_pub')
    while not rospy.is_shutdown():
        # Wait for event flag for new trajectory


        # Fetch trajectory
        local_data = data_in.get_data()
        local_frame_time = data_in.get_frame_time()
        angle, traveled_x, traveled_y = data_in.get_image_offset()

        # Split timestamp to secs and nsecs
        nsecs = local_frame_time % int(1000000000)
        secs = int((local_frame_time - nsecs)/1000000000)

        # Calculate world pose
        world_pose_x += traveled_x * PIXEL_SIZE
        world_pose_y += traveled_y * PIXEL_SIZE
        world_orientation = angle_add(world_orientation, angle)

        # Transform everything from camera frame to base frame
        traveled_x_base, traveled_y_base = transform_coordinates(traveled_x, traveled_y, transform_camera_to_base)
        world_pose_x_base, world_pose_y_base = transform_coordinates(world_pose_x, world_pose_y, transform_camera_to_base)

        # Publish transform from camera
        tf_msg = nav_msgs.Odometry()
        tf_msg.header.stamp.secs = secs
        tf_msg.header.stamp.nsecs = nsecs
        tf_msg.header.frame_id = "world_frame"
        tf_msg.child_frame_id = "vo"

        # Add twist
        tf_msg.twist.twist.linear.x = traveled_x_base
        tf_msg.twist.twist.linear.y = traveled_y_base
        tf_msg.twist.twist.linear.z = 0.0
        tf_msg.twist.twist.angular.x = 0.0
        tf_msg.twist.twist.angular.y = 0.0
        tf_msg.twist.twist.angular.z = angle
        tf_msg.twist.covariance = STANDARD_COVARIANCE

        # Add pose
        tf_msg.pose.pose.position.x = world_pose_x_base
        tf_msg.pose.pose.position.y = world_pose_y_base
        tf_msg.pose.pose.position.z = 0.0
        quat = tf_convert.quaternion_from_euler(0, 0, world_orientation)
        tf_msg.pose.pose.orientation.x = quat[0]
        tf_msg.pose.pose.orientation.y = quat[1]
        tf_msg.pose.pose.orientation.z = quat[2]
        tf_msg.pose.pose.orientation.w = quat[3]
        tf_msg.pose.covariance = STANDARD_COVARIANCE

        transform_pub.publish(tf_msg)

        #DEBUG
        print("Vision sent a transform to tf\n")

        while(not tf_buffer.can_transform('world_frame', 'camera_frame')):
            print("vision waiting for transform")
            time.sleep(1)

        # Get transform from camera to world for current image
        try:
            transform_camera_to_world = tf_buffer.lookup_transform('world_frame', 'camera_frame', rospy.Time(secs, nsecs))

            # Send each point in crack trajectory
            for path in local_data.path:
                # geometry_msgs PointStamped Message
                message = geo_msgs.PointStamped()
                message.header.frame_id = "world_frame"
                message.header.stamp.secs = secs
                message.header.stamp.nsecs = nsecs
                coords = transform_coordinates(path[0] * PIXEL_SIZE, path[1] * PIXEL_SIZE, transform_camera_to_world.transform)
                message.point.x = coords[0]
                message.point.y = coords[1]
                # Used for sending the end of crack information
                message.point.z = path[2]

                point_pub.publish(message)
                r.sleep()

        except (tf2_ros.LookupException, tf2_ros.ConnectivityException, tf2_ros.ExtrapolationException) as exception:
            print(exception)
            # For first transform publish again to ensure transform is available for first point
            r.sleep()
            continue

if __name__ == "__main__":
    vision_pub()