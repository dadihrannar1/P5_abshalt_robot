#include <ros/ros.h>
#include <tf2_ros/transform_broadcaster.h>
#include <tf2/LinearMath/Quaternion.h>
#include <tf2_geometry_msgs/tf2_geometry_msgs.h>
#include <nav_msgs/Odometry.h>
#include <std_msgs/Float64.h>>
#include <fstream>

const double WHEEL_DIAMETER = 0.38*M_PI;    //Wheel diameter
const double ENCODER_TICKS = 100;           //Ticks per encoder revolution
const double LENGTH_BETWEEN_WHEELS = 1.466; //Axle length in meters

struct FileData {
    std::vector<std::string> filenames;
    std::vector<int> time;
    std::vector<int> encoder1;
    std::vector<int> encoder2;
};

FileData read_JSON(const std::string& filepath) {
    std::fstream jsonfile;
    jsonfile.open(filepath,std::ios::in); //open a file to perform read operation using file object
    if (jsonfile.is_open()){ //checking whether the file is open
        // define variables
        std::string tp;
        FileData data;
        int j = 0;
        while(getline(jsonfile, tp, '"')){ //read data from file object and put it into string.
        if(tp == ", "){continue;} // if it's a ", " go next
        if(tp == "], ["){ // New file type
            j++;
            continue;
            }
        if(tp == "]]"){continue;} // End of document

        switch(j){
            case 0:
            data.filenames.push_back(tp);
            break;
            case 1:
            data.time.push_back(std::stoi(tp));
            break;
            case 2:
            data.encoder1.push_back(std::stoi(tp));
            break;
            case 3:
            data.encoder2.push_back(std::stoi(tp));
            break;
        }
      }
      jsonfile.close(); //close the file object.
      return data;
    }
    else{
        exit(10); //Could not open JSON file
    }
}

//Class to handle changes in world coordinates by reading wheel encoders
class DiffDrive{
private:
    double axle_length;     //Distance between wheel center points, in meters
    double wh_radius;       //Wheel radius, in meters
    double angle_per_tick;//Wheel angle per encoder tick, in radians

    //Translation and orientation of trailer in relation to world origin
    double world_x_trans = 0;
    double world_y_trans = 0;
    double world_z_rot = 0;

    //Velocities of trailer in relation to world origin
    double delta_x_trans = 0;
    double delta_y_trans = 0;
    double delta_z_rot = 0;

    //Function for adding angles bounded to [0, 2*PI[
    double angle_add(double angle_1, double angle_2){
        if(angle_1 + angle_2 >= 2*M_PI) return angle_1 + angle_2 - 2*M_PI;
        else if(angle_1 + angle_2 < 0) return angle_1 + angle_2 + 2*M_PI;
        else return angle_1 + angle_2;
    }
public:
    //Constructor for DiffDrive class
    DiffDrive(double wheel_radius, int encoder_increments, double length_between_wheels){
        //Calculate distance per tick for each encoder
        angle_per_tick = 2 * M_PI / encoder_increments;

        //Store length between wheels and wheel radius
        axle_length = length_between_wheels;
        wh_radius = wheel_radius;
    }

    //Method for reading encoder values and calculating new world coordinates
    void get_new_transform(int delta_r_encoder, int delta_l_encoder){
        //TODO: Somehow read encoder increments
        //int delta_r_encoder = 1;
        //int delta_l_encoder = 1;

        //Calculate angle changes
        double delta_r_angle = delta_r_encoder * angle_per_tick;
        double delta_l_angle = delta_l_encoder * angle_per_tick;

        //Calculate position change in local frame
        double local_delta_trans = wh_radius/2 * (delta_l_angle + delta_r_angle);

        //Calculate translation in global X Y
        delta_x_trans = local_delta_trans * cos(world_z_rot);
        delta_y_trans = local_delta_trans * sin(world_z_rot);

        //Calculate angular change around Z
        delta_z_rot = wh_radius/axle_length * (delta_l_angle - delta_r_angle);

        //Calculate vehicle movements in world coordinates
        world_x_trans += delta_x_trans;
        world_y_trans += delta_y_trans;
        world_z_rot = angle_add(world_z_rot, delta_z_rot);

        //Debug
        //ROS_INFO("\nX:\t%f\nY:\t%f\nAngle:\t%f", world_x_trans, world_y_trans, world_z_rot/M_PI*180);
    }

    //Method returns x position
    double get_x(){return world_x_trans;}

    //Method returns y position
    double get_y(){return world_y_trans;}

    //Method returns orientation as quaternion
    geometry_msgs::Quaternion get_quat(){
        //TF odometry orientation is 6DOF quaternion which is created from yaw angle
        tf2::Quaternion quat_tf;
        quat_tf.setRPY(0, 0, world_z_rot);
        quat_tf.normalize(); //

        //Convert quaternion datatype from tf to msg
        geometry_msgs::Quaternion quat_msg = tf2::toMsg(quat_tf);

        //Return quaternion message
        return quat_msg;
    }

    //Method returns x translation
    double get_delta_x(){return delta_x_trans;}

    //Method returns y translation
    double get_delta_y(){return delta_y_trans;}

    //Method returns angular change around z
    double get_delta_z_rot(){return delta_z_rot;}
};

static const boost::array<_Float64, 36> STANDARD_TWIST_COVARIANCE =
   {0.2461, 0, 0, 0, 0, 0,
    0, 0.2461, 0, 0, 0, 0,
    0, 0, 0.2461, 0, 0, 0,
    0, 0, 0, 0.2461, 0, 0,
    0, 0, 0, 0, 0.2461, 0,
    0, 0, 0, 0, 0, 0.2461};

//Vehicle speed callback for simulation
float vehicle_speed;
bool vehicle_speed_adjusted = false;
void vehicle_speed_callback(const std_msgs::Float64 &vehicle_vel){
    vehicle_speed = vehicle_vel.data;
    vehicle_speed_adjusted = true;
}

int main(int argc, char** argv){
    ros::init(argc, argv, "odometry_publisher");

    ros::NodeHandle n;
    ros::Publisher odom_pub = n.advertise<nav_msgs::Odometry>("odom", 50);
    tf2_ros::TransformBroadcaster odom_broadcaster;

    //Create differential drive handler
    DiffDrive ddr_position(WHEEL_DIAMETER, ENCODER_TICKS, LENGTH_BETWEEN_WHEELS);

    ros::Time current_time = ros::Time::now();

    ros::Subscriber vehicle_speed_sub = n.subscribe<std_msgs::Float64>("/vehicle_speed", 100, vehicle_speed_callback);
    FileData recorded_data = read_JSON("/media/sf_shared_files/Images_lang2/image_details.json");

    ros::Rate r(100);
    int previous_time = int(ros::Time::now().toNSec()/1e-6);
    
    //Iterate through the recorded data
    for(int i = 0; i < recorded_data.encoder1.size(); i++) {
        ros::spinOnce();    // check for incoming velocity messages

        //Wait until the next recorded timestamp from the arduino data
        if(!vehicle_speed_adjusted){
            while(previous_time > recorded_data.time[i]){sleep(0.1);}
            previous_time = recorded_data.time[i] + int(ros::Time::now().toNSec()/1e-6);
        }
        else{
            int time_to_next_encoder_tick = int((recorded_data.encoder1[i+1] * 0.38*M_PI/100) / vehicle_speed); //This may overflow at the last encoder increments

            while(previous_time > recorded_data.time[i] + time_to_next_encoder_tick){sleep(0.1);}
            previous_time = recorded_data.time[i] + time_to_next_encoder_tick + int(ros::Time::now().toNSec()/1e-6);
        }
        
        current_time = ros::Time::now();
        
        //Compute world coordinates
        ddr_position.get_new_transform(recorded_data.encoder1.at(i), recorded_data.encoder2.at(i)); //TODO read from JSON to get encoder ticks

        //Publish the odometry message over ROS
        nav_msgs::Odometry odom;
        odom.header.stamp = current_time;
        odom.header.frame_id = "world_frame";

        //Set the position
        odom.pose.pose.position.x = ddr_position.get_x();
        odom.pose.pose.position.y = ddr_position.get_y();
        odom.pose.pose.position.z = 0.0;
        odom.pose.pose.orientation = ddr_position.get_quat();
        odom.pose.covariance = STANDARD_TWIST_COVARIANCE;

        //set the velocity
        odom.child_frame_id = "base_link";
        odom.twist.twist.linear.x = ddr_position.get_delta_x();
        odom.twist.twist.linear.y = ddr_position.get_delta_y();
        odom.twist.twist.angular.z = ddr_position.get_delta_z_rot();
        odom.twist.covariance = STANDARD_TWIST_COVARIANCE;
        
        //publish the message
        odom_pub.publish(odom);

        r.sleep();
    }
}