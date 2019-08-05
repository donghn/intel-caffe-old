#Prepare data:
#Create raw lmdb of imagenet following https://github.com/intel/caffe/wiki/How-to-create-ImageNet-LMDB, please set "RESIZE=false" in  create_imagenet.sh to create raw lmdb
#ln -s ilsvrc12_val_lmdb "examples/imagenet/ilsvrc12_val_lmdb"

#expected result:
# loss3/top-1 = 0.75522
# loss3/top-5 = 0.926621
#-kind 0: 0x00 data, 1: 0xFF data, 2: Randomize data
#-temp 0: 60, 1: 40 2: 25
#-reten 0->6.
help()
{
	echo "Usage: $0 -k kind -t temperature -r retention_tim, -e is_ecc"
	echo -e "\t-k Kind of DRAM Error"
	echo -e "\t\t 0. use 0xFF and 0x00 error data, 1. use randomize error data "
	echo -e "\t-t Temperature of DRAM Measurement"
	echo -e "\t\t 0. 25*C, 1. 40*C, 2. 60*C"
	echo -e "\t-r Retention time of DRAM Measurement"
	echo -e "\t\t r value:          |   0   |   1   |   2  |  3  |  4  |  5  |  6   |"
	echo -e "\t\t retention time(s):| 0.128 | 0.256 | 0.512| 1.0 | 3.0 | 6.0 | 10.0 |"
	echo -e "\t-e Appy ECC or not"
	echo -e "\t\t 0. unable, 1. enable"
	echo -e "\t-f Flip negative value?"
	echo -e "\t\t 0. unable, 1. enable"
	echo -e "\t-h Show help"
	exit 1
}

while getopts ":k:t:r:e:f:h:" opt
do
	case $opt in
        	k) kind="$OPTARG";;
		t) temperature="$OPTARG";;
		r) retention_time="$OPTARG";;
		e) is_ecc="$OPTARG";;
		e) is_flip="$OPTARG";;
		h) help;;
		?) help;;
	esac
done

export DDR3_KIND=$kind
export DDR3_TEMP=$temperature
export DDR3_RETENTION=$retention_time
export DDR3_IS_ECC=$is_ecc
export DDR3_IS_FLIP=$is_flip
echo "Run with kind $DDR3_KIND, temperatue: $DDR3_TEMP , retention time: $DDR3_RETENTION , Ecc: $DDR3_IS_ECC, Flipping: $DDR3_IS_FLIP"


prototxt="./models/int8_resnet50/ResNet-50-deploy_quantized.prototxt"
#prototxt="./models/int8_resnet50/ResNet-50-train_val.prototxt"
weights="./models/int8_resnet50/ResNet-50-model.caffemodel"
inter=1000

#git pull https://donghn:nguyendong92@github.com/donghn/intel-caffe.git
#./scripts/build_intelcaffe.sh --compiler gcc
./build/tools/caffe test -model $prototxt -weights $weights --iterations $inter --engine MKLDNN
#/home/donghn/caffe/build/tools/caffe test -model $prototxt -weights $weights --iterations $inter
