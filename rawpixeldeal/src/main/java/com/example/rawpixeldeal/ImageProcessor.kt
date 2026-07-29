//package com.example.rawpixeldeal
//
//import android.graphics.Bitmap
//import org.opencv.android.Utils
//import org.opencv.core.*
//import org.opencv.imgproc.Imgproc
//import timber.log.Timber
//
//object ImageProcessor {
//
//    fun process(
//        bitmap: Bitmap,
//        contrast: Double,
//        brightness: Double,
//        sharpenDegree: Double,
//        bitWise: Boolean = false,
//        falseColor: Boolean = false,
//        relief: Boolean = false,
//        min: Double = 0.0,
//        max: Double = 100.0
//    ): Bitmap {
////        val brightnessPar = getScaledValue(brightness, -50.0, 50.0, min, max)
////        val contrastPar = getScaledValue(contrast, 0.7, 1.3, min, max)
////        val sharpenDegreePar = getScaledValue(sharpenDegree, 0.0, 1.0, min, max)
//
//        val src = convertToGrayScale(bitmap)
//
//        val bcResult = appBrightnessContrast(src, contrast, brightness)
//
//        var dst = applySharpen(bcResult, sharpenDegree)
//
//        applyInvertedColor(dst,bitWise)
//
//        applyFalseColor(dst,falseColor)
//
//        dst = applyEmbossingEffect(dst, relief)
//
//        return convertMatToBitmap(dst, bitmap.width, bitmap.height).also {
//            releaseMats(src, bcResult, dst)
//        }
//    }
//
//     fun convertToGrayScale(bitmap: Bitmap): Mat {
//        val src = Mat()
//        Utils.bitmapToMat(bitmap, src)
//        if (src.channels() > 1) {
//            Timber.tag("opencvProcess").d("COLOR_RGB2GRAY")
//            Imgproc.cvtColor(src, src, Imgproc.COLOR_RGB2GRAY)
//        }
//        return src
//    }
//
//    fun appBrightnessContrast(src: Mat, contrast: Double, brightness: Double): Mat {
//        if (contrast > 0 || (brightness != 0.0)){
//        val brightnessPar = getScaledValue(brightness, -100.0, 100.0, -100.0, 100.0)
//        val contrastPar = getScaledValue(contrast, 1.0, 1.8, 0.0, 100.0)
//        src.convertTo(src, src.type(), contrastPar, brightnessPar)
//        return src
//        }
//        return src
//    }
//
//    fun applySharpen(src: Mat, sharpen: Double): Mat {
//        if (sharpen > 0) {
//        val sharpenDegreePar = getScaledValue(sharpen, 0.0, 1.5, 0.0, 100.0)
//            val blurred = Mat()
//            Imgproc.GaussianBlur(src, blurred, Size(0.0, 0.0), 20.0)
//            val sharpened = Mat()
//            val alpha = 1.0 + sharpenDegreePar * 2.0  // 原图的权重，随着 sharpenDegreePar 增强
//            val beta = -sharpenDegreePar * 1.5  // 模糊图像的权重，随着 sharpenDegreePar 增强
//            Core.addWeighted(src, alpha, blurred, beta, 0.0, sharpened)
//            return sharpened
//        }
//        return src
//    }
//
//
//
//    fun applyInvertedColor(mat: Mat, invert: Boolean): Mat {
//        if (invert) {
//            Core.bitwise_not(mat, mat)
//            return mat
//        } else return mat
//    }
//
//    fun applyFalseColor(mat: Mat, falseColor: Boolean): Mat {
//        if (falseColor) {
//            Imgproc.applyColorMap(mat, mat, Imgproc.COLORMAP_JET)
//            return mat
//        } else return mat
//    }
//
//    fun applyEmbossingEffect(src: Mat, embossed: Boolean): Mat {
//        if (embossed) {
//            val blurred = Mat()
//            Imgproc.GaussianBlur(src, blurred, Size(3.0, 3.0), 0.0)
//
//            val xGrad = Mat()
//            val yGrad = Mat()
//            Imgproc.Sobel(blurred, xGrad, CvType.CV_32F, 1, 0, 3)
//            Imgproc.Sobel(blurred, yGrad, CvType.CV_32F, 0, 1, 3)
//
//            val dst = Mat()
//            Core.subtract(xGrad, yGrad, dst)
//            dst.convertTo(dst, CvType.CV_32F, 6.0, 110.0)
//            dst.convertTo(dst, CvType.CV_8UC1)
//
//            releaseMats(blurred, xGrad, yGrad)
//            return dst
//        } else return src
//    }
//
//    fun applyRotation(mat: Mat, angle: Double): Mat {
//        if (mat.empty() || mat.cols() == 0 || mat.rows() == 0) {
//            throw IllegalArgumentException("Input image is empty or has invalid dimensions")
//        }
//
//        if (angle == 0.0) {
//            return mat
//        }
//
//        val rotatedImage = Mat()
//
//        when (angle) {
//            90.0, -270.0 -> {
//                Core.transpose(mat, rotatedImage)
//                Core.flip(rotatedImage, rotatedImage, 1)
//            }
//            -90.0, 270.0 -> {
//                Core.transpose(mat, rotatedImage)
//                Core.flip(rotatedImage, rotatedImage, 0)
//            }
//            180.0, -180.0 -> {
//                Core.transpose(mat, rotatedImage)
//                Core.flip(mat, rotatedImage, -1)
//            }
//            else -> {
//                return mat
//            }
//        }
//
//        return rotatedImage
//    }
//
//
//    fun convertMatToBitmap(mat: Mat, width: Int, height: Int): Bitmap {
//        val resultBitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)
//        Utils.matToBitmap(mat, resultBitmap)
//        return resultBitmap
//    }
//
//    private fun releaseMats(vararg mats: Mat) {
//        mats.forEach { it.release() }
//    }
//
//    private fun getScaledValue(
//        value: Double,
//        a: Double = 0.0,
//        b: Double = 5.0,
//        rawMin: Double = 0.0,
//        rawMax: Double = 100.0
//    ): Double {
//        val k = (b - a) / (rawMax - rawMin)
//        return a + k * (value - rawMin)
//    }
//}
