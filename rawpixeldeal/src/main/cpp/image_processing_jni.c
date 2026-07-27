//#include <math.h>
//#include <stdlib.h>
//#include <stdio.h>
//#include <jni.h>
//#include "android/log.h"
//
//#define ENABLE_LOGGING  // 定义这个宏来启用日志
//#define TAG "image_processing_jni"
//#ifdef ENABLE_LOGGING
//#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__);
//#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__);
//#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__);
//#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__);
//#define LOGV(...) __android_log_print(ANDROID_LOG_VERBOSE, TAG, __VA_ARGS__);
//#else
//#define LOGD(...)
//#define LOGE(...)
//#define LOGI(...)
//#define LOGW(...)
//#define LOGV(...)
//#endif
//
//
//// 计算两个相邻点的斜率
//double calculate_slope(double * smoothed_histogram, int idx1, int idx2) {
//    return (double)(smoothed_histogram[idx2] - smoothed_histogram[idx1]);
//}
//
//// 计算三点斜率
//double calculate_three_point_slope(const double * smoothed_histogram, int idx, int max_val) {
//    if (idx < 1 || idx > (max_val - 1)) {
//        return 0.0;  // 避免访问越界
//    }
//
//    double slope = (double)(smoothed_histogram[idx + 1] - smoothed_histogram[idx]) +
//                   (double)(smoothed_histogram[idx] - smoothed_histogram[idx - 1]);
//
//    return slope / 2.0;  // 平均两段斜率
//}
//
//
//// 寻找上升沿和下降沿
//void find_edges(double * smoothed_histogram, int length, int main_peak, int* min_idx, int* max_idx,
//                int max_val) {
//    // 计算左侧的上升沿
//    int left_min_idx = -1;
//    for (int i = main_peak - 1; i >= 0; i--) {
//        if (smoothed_histogram[i] < smoothed_histogram[main_peak] * 0.55) {
//            left_min_idx = i;
//            break;  // 找到第一个低于阈值的点
//        }
//    }
//
//    // 向左计算斜率，直到满足条件（斜率小于1.0）
//    if (left_min_idx != -1) {
//        for (int i = left_min_idx - 1; i >= 0; i--) {
////            double slope = calculate_slope(smoothed_histogram, i, i + 1);
//            double slope = calculate_three_point_slope(smoothed_histogram, i, max_val);
//            if (slope < 10.0) {
//                *min_idx = i;
//                break;
//            }
//        }
//    }
//
//    // 计算右侧的下降沿
//    int right_max_idx = -1;
//    for (int i = main_peak + 1; i < length; i++) {
//        if (smoothed_histogram[i] < smoothed_histogram[main_peak] * 0.55) {
//            right_max_idx = i;
//            break;  // 找到第一个低于阈值的点
//        }
//    }
//
//    // 向右计算斜率，直到满足条件（斜率大于1.0）
//    if (right_max_idx != -1) {
//        for (int i = right_max_idx + 1; i < length; i++) {
////            double slope = calculate_slope(smoothed_histogram, i - 1, i);
//            double slope = calculate_three_point_slope(smoothed_histogram, i, max_val);
//            if (slope > -10.0) {
//                *max_idx = i;
//                break;
//            }
//        }
//    }
//}
//
//// 计算波峰的宽度（基于50%阈值）
//void find_peak_width(const double * smoothed_histogram, int peak_idx, int length, int* left_idx,
//                     int* right_idx) {
//    double peak_height = smoothed_histogram[peak_idx];
//    double threshold = peak_height * 0.5;
//
//    // 向左寻找50%高度阈值
//    *left_idx = peak_idx;
//    while (*left_idx > 0 && smoothed_histogram[*left_idx] > threshold) {
//        (*left_idx)--;
//    }
//
//    // 向右寻找50%高度阈值
//    *right_idx = peak_idx;
//    while (*right_idx < length - 1 && smoothed_histogram[*right_idx] > threshold) {
//        (*right_idx)++;
//    }
//}
//
//// 计算波峰的高度和宽度的乘积
//double compute_peak_height_width_product(double * smoothed_histogram, int peak_idx, int length) {
//    int left_idx, right_idx;
//    find_peak_width(smoothed_histogram, peak_idx, length, &left_idx, &right_idx);
//
//    // 波峰高度
//    double peak_height = smoothed_histogram[peak_idx];
//
//    // 波峰宽度
//    int peak_width = right_idx - left_idx + 1;
//
//    // 返回高度和宽度的乘积
//    return peak_height * peak_width;
//}
//
//// 寻找波峰并选择高度和宽度乘积最大的波峰
//int find_peak_with_max_height_width_product(double * smoothed_histogram, int length) {
//    int max_peak_idx = 0;
//    double max_product = 0.0;
//
//    for (int i = 1; i < length - 1; i++) {
//        // 检查是否是波峰（比左右相邻的点大）
//        if (smoothed_histogram[i] > smoothed_histogram[i - 1] && smoothed_histogram[i] > smoothed_histogram[i + 1]) {
//            double product = compute_peak_height_width_product(smoothed_histogram, i, length);
//
//            // 选择高度和宽度乘积最大的波峰
//            if (product > max_product) {
//                max_product = product;
//                max_peak_idx = i;
//            }
//        }
//    }
//    // 返回高度和宽度乘积最大的波峰的索引
//    LOGD("max_peak_idx:%d\n", max_peak_idx)
//    return max_peak_idx;
//}
//
//void compute_gaussian_kernel(double **kernel, int *kernel_size, double sigma) {
//    int radius = (int)round(4 * sigma);
//    *kernel_size = 2 * radius + 1;
//
//    // 动态分配内存
//    *kernel = (double*)malloc(*kernel_size * sizeof(double));
//    if (*kernel == NULL) {
//        fprintf(stderr, "Memory allocation failed\n");
//        exit(1);
//    }
//
//    // 生成高斯核
//    double sum = 0.0;
//    for (int i = 0; i < *kernel_size; i++) {
//        int x = i - radius;
//        (*kernel)[i] = exp(-0.5 * (x * x) / (sigma * sigma));
//        sum += (*kernel)[i];
//    }
//
//    // 归一化
//    for (int i = 0; i < *kernel_size; i++) {
//        (*kernel)[i] /= sum;
//    }
//}
//
//void gaussian_smooth(const int *hist, double *smooth_hist, int hist_size, double sigma) {
//    double *kernel;
//    int kernel_size;
//    compute_gaussian_kernel(&kernel, &kernel_size, sigma);
//    int radius = (kernel_size - 1) / 2;
//
//    for (int i = 0; i < hist_size; i++) {
//        double sum = 0.0;
//        for (int j = -radius; j <= radius; j++) {
//            int idx = i + j;
//
//            while (idx < 0 || idx >= hist_size) {
//                if (idx < 0) {
//                    idx = -idx - 1;
//                } else {
//                    idx = 2 * hist_size - idx - 1;
//                }
//            }
//
//            sum += hist[idx] * kernel[j + radius];
//        }
//        smooth_hist[i] = sum;
//    }
//
//    free(kernel);
//}
//
//// 计算直方图和分桶边界
//void histogram_calculations(const int* pixel_array, int array_length, int* histogram, int* bin_edges,
//                            int max_value, double num_bins) {
//    if (num_bins > max_value) {
//        num_bins = max_value;
//    }
//    // 计算每个桶的宽度
//    double bin_width = (double )(max_value + 1) / num_bins;
//
//    for (int i = 0; i < array_length; i++) {
//        int pixel_value = pixel_array[i];
//        if (pixel_value >= 0 && pixel_value <= max_value) {
//            int bin_index = (int)(pixel_value / bin_width);
//            histogram[bin_index]++;
//        }
//    }
//
//    for (int i = 0; i <= num_bins; i++) {
//        bin_edges[i] = (int)(i * bin_width);
//    }
//}
//
//void auto_window_level(const int* pixel_array, int* replyData, int array_length) {
//    FILE *pixel_arrayFp = fopen("/data/tmp/pixel_array.txt", "w");
//    if (pixel_arrayFp) {
//        for (int i = 0; i < array_length; i++) {
//            fprintf(pixel_arrayFp, "%d\n", pixel_array[i]);
//        }
//        fclose(pixel_arrayFp);
//        LOGD("%s:histogram data saved to /data/tmp/pixel_array.txt", __func__)
//    } else {
//        LOGE("%s:Failed to save pixel_array data", __func__)
//    }
//    int max_value = pixel_array[0];
//    for (int i = 1; i < array_length; i++) {
//        if (pixel_array[i] > max_value) {
//            max_value = pixel_array[i];
//        }
//    }
//    replyData[0] = max_value;
//    LOGD("%s:max_value:%d\n", __func__, max_value)
////    int* histogram = (int*)calloc(max_value + 1, sizeof(int));
////    int* bin_edges = (int*)calloc(max_value + 2, sizeof(int));
////    histogram_calculations(pixel_array,array_length,histogram,bin_edges,max_value,1000);
//
//    // 动态分配内存
//    int num_bins = 500;
//    int* histogram = (int*)calloc(num_bins, sizeof(int));
//    int* bin_edges = (int*)calloc(num_bins + 1, sizeof(int));
//    if (histogram == NULL || bin_edges == NULL) {
//        // 内存分配失败的处理
//        LOGE("Memory allocation failed")
//        if (histogram) free(histogram);
//        if (bin_edges) free(bin_edges);
//        return;
//    }
//    histogram_calculations(pixel_array, array_length, histogram, bin_edges, max_value, num_bins);
//
//    FILE *histogramFp = fopen("/data/tmp/histogram.txt", "w");
//    if (histogramFp) {
//        for (int i = 0; i < num_bins; i++) {
//            fprintf(histogramFp, "%d\n", histogram[i]);
//        }
//        fclose(histogramFp);
//        LOGD("%s:histogram data saved to /data/tmp/histogram.txt", __func__)
//    } else {
//        LOGE("%s:Failed to save histogram data", __func__)
//    }
//    double * smoothed_histogram = (double *)malloc((num_bins) * sizeof(double));
//    // 转换为 double
////    double *hist_double = (double*)malloc((max_value + 1) * sizeof(double));
////    for(int i = 0; i < max_value; i++) {
////        hist_double[i] = (double)histogram[i];
////    }
//    gaussian_smooth(histogram, smoothed_histogram, num_bins, 20.0);
//
//    // 新增代码：保存平滑数据到文件
//    FILE *smoothed_histogramFp = fopen("/data/tmp/smoothed_data.txt", "w");
//    if (smoothed_histogramFp) {
//        for (int i = 0; i < num_bins; i++) {
//            fprintf(smoothed_histogramFp, "%.2f\n", smoothed_histogram[i]);
//        }
//        fclose(smoothed_histogramFp);
//        LOGD("%s:Smoothed data saved to /data/tmp/smoothed_data.txt", __func__)
//    } else {
//        LOGE("%s:Failed to save smoothed data", __func__)
//    }
//
//    // 寻找最大波峰（基于波峰面积）
//    int main_peak = find_peak_with_max_height_width_product(smoothed_histogram, num_bins);
//
//    // 寻找波峰两侧的上升沿和下降沿
//    int min_idx = 0, max_idx = num_bins + 1;
//    find_edges(smoothed_histogram, num_bins, main_peak, &min_idx, &max_idx, num_bins - 1);
//
//    // 根据找到的上升沿和下降沿计算 wc 和 ww
//    LOGD("%s:max:%d,main_peak:%d\n.", __func__, replyData[0], main_peak)
//    int wc = (bin_edges[max_idx + 1] + bin_edges[min_idx])/2;
//    int ww = bin_edges[max_idx + 1] - bin_edges[min_idx];
//    replyData[1] = wc;
//    replyData[2] = ww;
//
//    LOGD("%s:max:%d ,wc:%d, ww:%d\n.", __func__, replyData[0], replyData[1], replyData[2])
//    free(histogram);
//    free(bin_edges);
//    free(smoothed_histogram);
//}
//
//// JNI 方法
//JNIEXPORT jintArray JNICALL
//Java_com_urit_dentalfilmmate_util_ImageProcessingJni_autoWindowLevel(JNIEnv *env, jobject thiz,
//                                   jintArray pixel_array,jint array_length) {
//    jint* cArray = (*env)->GetIntArrayElements(env, pixel_array, NULL);
//    if (cArray == NULL) {
//        return NULL;
//    }
//    int replyData[3];
//    auto_window_level(cArray, replyData, array_length);
//    (*env)->ReleaseIntArrayElements(env, pixel_array, cArray, 0);
//    jintArray result = (*env)->NewIntArray(env, 3);
//    if (result == NULL) {
//        LOGE("Failed to create JNI int array");
//        return NULL;
//    }
//    (*env)->SetIntArrayRegion(env, result, 0, 3, replyData);
//    return result;
//}
