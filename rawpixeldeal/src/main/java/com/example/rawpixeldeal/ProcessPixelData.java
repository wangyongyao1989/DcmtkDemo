//package com.example.rawpixeldeal;
//
//import android.graphics.Bitmap;
//
//import com.example.rawpixeldeal.xray.ImageProcessingJni;
//import com.example.rawpixeldeal.xray.LogUtil;
//import com.example.rawpixeldeal.xray.PixelData;
//
//import org.opencv.core.Core;
//import org.opencv.core.CvType;
//import org.opencv.core.Mat;
//import org.opencv.core.MatOfFloat;
//import org.opencv.core.MatOfPoint;
//import org.opencv.core.MatOfPoint2f;
//import org.opencv.core.Point;
//import org.opencv.core.Rect;
//import org.opencv.core.RotatedRect;
//import org.opencv.core.Size;
//import org.opencv.imgproc.Imgproc;
//
//import java.util.ArrayList;
//import java.util.List;
//
///**
// * 老版 ProcessPixelData（设计参考），与 readme.md 描述的 5 步管线一致。
// *
// * 本版本：
// *  - 引用了 rawpixeldeal.xray.{LogUtil, ImageProcessingJni, PixelData}
// *  - 保留与 readme §4.1 相同的 tailor_img 子步骤
// *  - 不依赖 dcmtk 模块（避免循环依赖）
// *
// * 新代码请直接用 {@link com.example.rawpixeldeal.xray.XrayPipeline}，更现代、更多日志、
// * 自动写 DICOM。
// */
//public class ProcessPixelData {
//
//    private static final String TAG = "ProcessPixelData";
//
//    // ---------- readme §5 "魔法数" ----------
//    private static final int SENSOR_DEPTH = 65536;
//    private static final double CONTRAST_RATIO = 0.65;
//    private static final double THIN = 0.8;
//    private static final double MIDDLE = 2.0;
//    private static final double THICK = 2.8;
//    private static final double GAMMA = 0.75;
//    private static final double GAMMA_PLUS = 2.25;
//    private static final double CLIP_MIN = 0.05;
//    private static final double CLIP_MAX = 0.05;
//    private static final int SIGMA = 3;
//    private static final int W = 2;
//    private static final int MARGIN = 0;
//    private static final int UPPER = 0;
//    private static final int BOTTOM = 0;
//    private static final int MIN_AREA_THRESHOLD = 40000;
//
//    /**
//     * readme §4.1 tailor_img 多阶段裁剪（旋转 + 锐化 + Sobel + 闭运算 + boundingRect）。
//     */
//    public static Mat tailor_img(Mat img_mat_org_in, int[] img_org) {
//        Mat img_mat_org = img_mat_org_in.clone();
//        // 1) 16→8 降级
//        Mat img_mat_org1 = new Mat();
//        img_mat_org.convertTo(img_mat_org1, CvType.CV_8UC1);
//        Core.convertScaleAbs(img_mat_org1, img_mat_org1, 1.0 / 256, 0);
//        Mat img_mat_org2 = new Mat();
//        img_mat_org.convertTo(img_mat_org2, CvType.CV_8UC1);
//        // 2) OTSU
//        Imgproc.threshold(img_mat_org1, img_mat_org1, 10, 255, Imgproc.THRESH_OTSU);
//        // 3) 最大外轮廓
//        List<MatOfPoint> contours = new ArrayList<>();
//        Mat hierarchy = new Mat();
//        Imgproc.findContours(img_mat_org1, contours, hierarchy,
//                Imgproc.RETR_EXTERNAL, Imgproc.CHAIN_APPROX_SIMPLE);
//        double max_area = 0;
//        int maxAreaIdx = -1;
//        for (int i = 0; i < contours.size(); i++) {
//            double area = Imgproc.contourArea(contours.get(i));
//            if (area > max_area) {
//                max_area = area;
//                maxAreaIdx = i;
//            }
//        }
//        if (maxAreaIdx < 0) {
//            LogUtil.w(TAG, "tailor_img: no contours, fallback to original");
//            return img_mat_org;
//        }
//        // 4) minAreaRect 角度归一化
//        MatOfPoint2f contour2f = new MatOfPoint2f(contours.get(maxAreaIdx).toArray());
//        RotatedRect rr = Imgproc.minAreaRect(contour2f);
//        double angle = rr.angle;
//        if (Math.abs(angle) > 45) angle += 90;
//        LogUtil.d(TAG, "tailor_img: minAreaRect angle=" + angle);
//        // 5) 旋转校正
//        if (Math.abs(angle) > 0.5 && max_area > MIN_AREA_THRESHOLD) {
//            Point center = new Point(img_mat_org.width() / 2.0,
//                    img_mat_org.height() / 2.0);
//            Mat M = Imgproc.getRotationMatrix2D(center, angle, 1.0);
//            Imgproc.warpAffine(img_mat_org, img_mat_org, M, img_mat_org.size());
//            Imgproc.warpAffine(img_mat_org2, img_mat_org2, M, img_mat_org.size());
//        }
//        // 6) 3×3 锐化核（center=9）
//        Mat kernel = new MatOfFloat(
//                -1, -1, -1,
//                -1,  9, -1,
//                -1, -1, -1
//        ).reshape(1, 3);
//        Mat img_dst = new Mat();
//        Imgproc.filter2D(img_mat_org2, img_dst, -1, kernel, new Point(-1, -1), 0);
//        // 7) Sobel x-y 差异
//        Mat grad_x = new Mat();
//        Mat grad_y = new Mat();
//        Mat gradient_xy = new Mat();
//        Imgproc.Sobel(img_dst, grad_x, CvType.CV_16S, 1, 0);
//        Imgproc.Sobel(img_dst, grad_y, CvType.CV_16S, 0, 1);
//        Core.subtract(grad_x, grad_y, gradient_xy);
//        // 8) blur + OTSU + 膨胀
//        Mat blurred = new Mat();
//        Imgproc.blur(gradient_xy, blurred, new Size(25, 25));
//        Mat thresh = new Mat();
//        Imgproc.threshold(blurred, thresh, 0, 255, Imgproc.THRESH_BINARY | Imgproc.THRESH_OTSU);
//        Imgproc.dilate(thresh, thresh, new Mat(), new Point(-1, -1), 4);
//        // 9) MORPH_CROSS 闭运算
//        Mat morpho_kernel = Imgproc.getStructuringElement(
//                Imgproc.MORPH_CROSS, new Size(25, 25));
//        Mat closed = new Mat();
//        Imgproc.morphologyEx(thresh, closed, Imgproc.MORPH_CLOSE, morpho_kernel);
//        // 10) boundingRect
//        List<MatOfPoint> contours2 = new ArrayList<>();
//        Mat hierarchy2 = new Mat();
//        Imgproc.findContours(closed, contours2, hierarchy2,
//                Imgproc.RETR_EXTERNAL, Imgproc.CHAIN_APPROX_SIMPLE);
//        double max_area2 = 0;
//        int maxAreaIdx2 = -1;
//        for (int i = 0; i < contours2.size(); i++) {
//            double area = Imgproc.contourArea(contours2.get(i));
//            if (area > max_area2) {
//                max_area2 = area;
//                maxAreaIdx2 = i;
//            }
//        }
//        if (maxAreaIdx2 < 0) {
//            LogUtil.w(TAG, "tailor_img: 2nd pass no contours, fallback to original");
//            return img_mat_org;
//        }
//        Rect bounding_rect = Imgproc.boundingRect(contours2.get(maxAreaIdx2));
//        LogUtil.d(TAG, "tailor_img: boundingRect=" + bounding_rect);
//        if (bounding_rect.area() < MIN_AREA_THRESHOLD) {
//            LogUtil.w(TAG, "tailor_img: boundingRect too small, fallback to original");
//            return img_mat_org;
//        }
//        // 11) 返回裁剪区域
//        Mat cropped = new Mat(img_mat_org, bounding_rect);
//        return cropped;
//    }
//
//    /**
//     * readme §4.3 主入口 5 步管线。
//     *
//     * 注意：本方法与 readme 描述一致，但内部已重写为可编译版本：
//     *  - 使用 xray.ImageProcessingJni.autoWindowLevel 替代原缺失的 JNI 类
//     *  - 使用 xray.PixelData 替代原缺失的 8 参构造
//     *  - 使用 xray.LogUtil 替代原缺失的 LogUtil
//     */
//    public static PixelData process(byte[] pixelData, Integer imageWidth, Integer imageHeight) {
//        // 1) raw → 16-bit
//        int w = imageWidth;
//        int h = imageHeight;
//        int[] data16 = new int[w * h];
//        for (int i = 0; i < w * h; i++) {
//            data16[i] = ((pixelData[2 * i] & 0xFF) << 8) | (pixelData[2 * i + 1] & 0xFF);
//        }
//        Mat img_mat_org = new Mat(h, w, CvType.CV_32S);
//        img_mat_org.put(0, 0, data16);
//
//        // 2) tailor_img
//        Mat img_mat_cut = tailor_img(img_mat_org, data16);
//
//        // 3) 二次裁剪 + 转 float
//        int img_cutted_width = img_mat_cut.width();
//        int img_cutted_height = img_mat_cut.height();
//        Rect rect = new Rect(
//                MARGIN, MARGIN + UPPER,
//                img_cutted_width - MARGIN,
//                img_cutted_height - 2 * MARGIN - BOTTOM);
//        if (rect.width <= 0 || rect.height <= 0) {
//            rect = new Rect(0, 0, img_cutted_width, img_cutted_height);
//        }
//        Mat img_mat_cut2 = new Mat(img_mat_cut, rect);
//        Mat img_mat_cut_float = new Mat();
//        img_mat_cut2.convertTo(img_mat_cut_float, CvType.CV_32F);
//
//        // 4) 16-bit 直方图 + 归一化 CDF
//        int arrayLength = img_mat_cut_float.rows() * img_mat_cut_float.cols();
//        int[] img_data_16b = new int[arrayLength];
//        int largestImagePixelValue = 0;
//        for (int i = 0; i < arrayLength; i++) {
//            float v = (float) img_mat_cut_float.get(i / img_mat_cut_float.cols(),
//                    i % img_mat_cut_float.cols())[0];
//            int iv = (int) v;
//            if (iv > 65535) iv = 65535;  // 防止越界
//            img_data_16b[i] = iv;
//            if (iv > largestImagePixelValue) largestImagePixelValue = iv;
//        }
//        int[] img_histogram = new int[SENSOR_DEPTH + 1];
//        for (int v : img_data_16b) {
//            img_histogram[v]++;
//        }
//        int totalPix = img_data_16b.length;
//        double[] img_histogram_calculus = new double[SENSOR_DEPTH + 1];
//        int cum = 0;
//        for (int i = 0; i <= SENSOR_DEPTH; i++) {
//            cum += img_histogram[i];
//            img_histogram_calculus[i] = (double) cum / totalPix;
//        }
//        int min_i = 0, max_i = SENSOR_DEPTH;
//        for (int i = 0; i <= SENSOR_DEPTH; i++) {
//            if (img_histogram_calculus[i] > CLIP_MIN) { min_i = i; break; }
//        }
//        for (int i = SENSOR_DEPTH; i >= 0; i--) {
//            if (img_histogram_calculus[i] > 1 - CLIP_MAX) { max_i = i; break; }
//        }
//        LogUtil.d(TAG, "process: min_i=" + min_i + " max_i=" + max_i);
//
//        // 5) autoWindowLevel
//        int[] retArr = ImageProcessingJni.autoWindowLevel(img_data_16b, arrayLength);
//        int largestPV2 = retArr[0];
//        int win_center = retArr[1];
//        int win_width = retArr[2];
//
//        // 6) exposure / stddev
//        long sum = 0;
//        for (int v : img_data_16b) sum += v;
//        int exposure_level = (int) (sum / img_data_16b.length);
//        double varianceSum = 0;
//        for (int v : img_data_16b) varianceSum += Math.pow(v - exposure_level, 2);
//        double standardDeviation = Math.sqrt(varianceSum / img_data_16b.length);
//
//        // 7) 输出大端 16-bit raw
//        byte[] out_pixelData = new byte[arrayLength * 2];
//        for (int i = 0; i < img_data_16b.length; i++) {
//            out_pixelData[2 * i] = (byte) ((img_data_16b[i] >> 8) & 0xFF);
//            out_pixelData[2 * i + 1] = (byte) (img_data_16b[i] & 0xFF);
//        }
//        return new PixelData(
//                img_cutted_height, img_cutted_width,
//                out_pixelData, largestPV2,
//                win_center, win_width,
//                exposure_level, standardDeviation);
//    }
//
//    /**
//     * 便捷方法：把 X-ray 处理结果应用到 raw 数据并生成 Bitmap。
//     */
//    public static Bitmap processAndBitmap(byte[] pixelData, Integer imageWidth, Integer imageHeight) {
//        PixelData pd = process(pixelData, imageWidth, imageHeight);
//        int w = pd.getWidth();
//        int h = pd.getHeight();
//        int c = pd.getWinCenter();
//        int width = pd.getWinWidth();
//        int lo = c - width / 2;
//        int hi = c + width / 2;
//        int range = Math.max(1, hi - lo);
//        int[] argb = new int[w * h];
//        if (pd.getData() != null) {
//            byte[] raw = pd.getData();
//            for (int i = 0; i < w * h && 2 * i + 1 < raw.length; i++) {
//                int v = ((raw[2 * i] & 0xFF) << 8) | (raw[2 * i + 1] & 0xFF);
//                int clamped = Math.max(lo, Math.min(hi, v));
//                int gray = (int) ((double) (clamped - lo) / range * 255.0);
//                if (gray < 0) gray = 0;
//                if (gray > 255) gray = 255;
//                argb[i] = 0xFF000000 | (gray << 16) | (gray << 8) | gray;
//            }
//        }
//        return Bitmap.createBitmap(argb, w, h, Bitmap.Config.ARGB_8888);
//    }
//}
