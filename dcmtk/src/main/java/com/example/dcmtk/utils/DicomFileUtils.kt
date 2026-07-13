// 已用 DCMTK native 重新实现，见 DcmtkJni.kt；原 dcm4che3 参考实现整体注释（依赖未引入，无法编译）
//
//import android.graphics.Bitmap
//import org.dcm4che3.android.Raster
//import org.dcm4che3.android.RasterUtil
//import org.dcm4che3.android.imageio.dicom.DicomImageReadParam
//import org.dcm4che3.android.imageio.dicom.DicomImageReader
//import org.dcm4che3.data.Attributes
//import org.dcm4che3.data.Tag
//import org.dcm4che3.data.UID
//import org.dcm4che3.data.VR
//import org.dcm4che3.io.DicomInputStream
//import org.dcm4che3.io.DicomOutputStream
//import org.dcm4che3.util.UIDUtils
//import timber.log.Timber
//import java.io.File
//import java.io.IOException
//import java.text.SimpleDateFormat
//import java.util.Date
//import java.util.Locale
//import kotlin.math.min
//
//fun loadDicomFileInfo(file: File): HashMap<String, String> {
//    val hashMap = hashMapOf<String, String>()
//    val info = StringBuilder()
//    info.append("dicom文件：${file.name}" + "\n")
//    try {
//        val dcmInputStream = DicomInputStream(file)
//        val attrs: Attributes = dcmInputStream.readDataset(-1, -1)
////        val originalCharset = attrs.getString(Tag.SpecificCharacterSet, "ISO_IR 192")
////        LogUtil.d("原始字符集：$originalCharset")
////        attrs.setString(Tag.SpecificCharacterSet, VR.CS, originalCharset)
////        LogUtil.d("输出所有属性信息2:$attrs")
//        val exposureIndex: String = attrs.getString(Tag.ExposureIndex, "")
//        hashMap["ExposureIndex"] = exposureIndex
//        val patientName: String = attrs.getString(Tag.PatientName, "")
//        info.append("姓名：$patientName\n")
//        hashMap["PatientName"] = patientName
//        // 生日
//        val patientBirthDate: String = attrs.getString(Tag.PatientBirthDate, "")
//        info.append("生日：$patientBirthDate\n")
//        hashMap["PatientBirthDate"] = patientBirthDate
//        // 机构
//        val institution: String = attrs.getString(Tag.InstitutionName, "")
//        info.append("机构：$institution\n")
//        hashMap["InstitutionName"] = institution
//        // 站点
//        val station: String = attrs.getString(Tag.StationName, "")
//        info.append("站点：$station" + "\n")
//
//        // 制造商
//        val Manufacturer: String = attrs.getString(Tag.Manufacturer, "")
//        info.append("制造商：$Manufacturer" + "\n")
//
//        // 制造商模型
//        val ManufacturerModelName: String = attrs.getString(Tag.ManufacturerModelName, "")
//        info.append("制造商模型：$ManufacturerModelName" + "\n")
//
//        val description: String = attrs.getString(Tag.StudyDescription, "")
//        info.append("StudyDescription：$description" + "\n")
//        val SeriesDescription: String = attrs.getString(Tag.SeriesDescription, "")
//        info.append("SeriesDescription：$SeriesDescription" + "\n")
//
//        // 描述时间
////        val studyData: String = attrs.getString(Tag.StudyDate, "")
////        info.append("描述时间：$studyData" + "\n")
//
//        val studyDate = attrs.getString(Tag.StudyDate) ?: ""
//        val studyTime = attrs.getString(Tag.StudyTime)?.padEnd(6, '0') ?: "000000"
//        hashMap["StudyDate"] = if (studyDate.length == 8 && studyTime.length >= 6) {
//            "${studyDate.substring(0..3)}-${studyDate.substring(4..5)}-${studyDate.substring(6..7)} " +
//                    "${studyTime.substring(0..1)}:${studyTime.substring(2..3)}:${
//                        studyTime.substring(
//                            4..5
//                        )
//                    }"
//        } else {
//            "0000-00-00 00:00:00"
//        }
//        hashMap["PatientID"] = attrs.getString(Tag.PatientID) ?: ""
//        hashMap["StudyID"] = attrs.getString(Tag.StudyID) ?: ""
//        hashMap["PatientAge"] = attrs.getString(Tag.PatientAge) ?: ""
//        hashMap["PatientSex"] = when (attrs.getString(Tag.PatientSex)) {
//            "M" -> "男"
//            "F" -> "女"
//            else -> ""
//        }
//        hashMap["BodyPartExamined"] = attrs.getString(Tag.BodyPartExamined) ?: ""
//    } catch (e: Exception) {
//        Timber.e("" + e)
//        info.append(e.message)
//    }
////    LogUtil.d(info.toString())
//    return hashMap
//}
//
//fun extractAllDicomFields(file: File): HashMap<String, String> {
//    val hashMap = HashMap<String, String>()
//    try {
//        DicomInputStream(file).use { dcmInputStream ->
//            val attrs: Attributes = dcmInputStream.readDataset(-1, -1)
//            val originalCharset = attrs.getString(Tag.SpecificCharacterSet, "ISO_IR 192")
//            attrs.setString(Tag.SpecificCharacterSet, VR.CS, originalCharset)
//
//            for (tag in attrs.tags()) {
//                val tagName = tag.toString()
//                val value = attrs.getString(tag, "")
//                hashMap[tagName] = value
//            }
//            LogUtil.d("提取的所有字段信息：$hashMap")
//        }
//    } catch (e: Exception) {
//        Timber.e("读取 DICOM 文件时出错：${e.message}")
//    }
//    return hashMap
//}
//
///**
// * 读取文件数据
// */
//fun loadDicomFileDefaultWindowWidthWindowCenter(file: File): IntArray {
//    val info = StringBuilder()
//    info.append("dicom文件：${file.name}" + "\n")
//    DicomInputStream(file).use {
//        // 属性对象
//        val attrs: Attributes = it.readDataset(-1, -1)
//        // 窗宽窗位
//        val largestImagePixelValue = attrs.getInt(Tag.LargestImagePixelValue, 16383)
//        val defaultWindowCenter = attrs.getInt(Tag.WindowCenter, 1)
//        val defaultWindowWidth = attrs.getInt(Tag.WindowWidth, 1)
//        return intArrayOf(largestImagePixelValue, defaultWindowCenter, defaultWindowWidth)
//    }
//}
//
//
///**
// * 从DICOM文件中读取窗宽窗位(WW/WL)和像素范围信息
// * 符合DICOM标准，支持多值WW/WL和VOI LUT Sequence
// *
// *  此方法用同一个DICOM文件在DICOM Viewer标准工具上验证获取的WW/WL数值一致
// *  DICOM Viewer连接为：https://www.imaios.com/cn/imaios-dicom-viewer
// *
// * @param file DICOM文件
// * @return 窗宽窗位及像素范围信息
// * @throws IOException 文件读取失败或DICOM格式错误
// */
//@Throws(IOException::class)
//fun readDicomWindowSettings(file: File): DicomWindowSettings {
//    DicomInputStream(file).use { dis ->
//        // ✅ 关键优化：只读取元数据，读到PixelData标签就停止
//        // 避免读取几MB甚至几十MB的像素数据，大幅提升大文件读取速度
//        val attrs: Attributes = dis.readDataset(-1, Tag.PixelData)
//        //LogUtil.w("✅ 成功读取DICOM元数据（已跳过PixelData）")
//
//        // 读取像素值范围（必须成对存在）
//        // CT常用12位=0~4095，DR常用16位=0~65535
//        val smallestPixelValue = attrs.getInt(Tag.SmallestImagePixelValue, 0)
//        val largestPixelValue = attrs.getInt(Tag.LargestImagePixelValue, 4095)
//        //LogUtil.w("📊 读取像素值范围:")
//        //LogUtil.w("  SmallestImagePixelValue(0028,0106): $smallestPixelValue")
//        //LogUtil.w("  LargestImagePixelValue(0028,0107): $largestPixelValue")
//
//        // 自动计算默认窗宽窗位（当文件无任何预设时使用）
//        val autoWindow = DicomWindowSettings.DicomWindow(
//            center = (smallestPixelValue + largestPixelValue).toDouble() / 2.0,
//            width = (largestPixelValue - smallestPixelValue).toDouble(),
//            description = "自动计算"
//        )
//        //LogUtil.w("🧮 自动计算默认窗宽窗位: WL=${autoWindow.center}, WW=${autoWindow.width}")
//
//        val windows = mutableListOf<DicomWindowSettings.DicomWindow>()
//
//        // 第一步：优先读取标准的WindowCenter/WindowWidth标签（支持多值）
//        //LogUtil.w("\n🔍 尝试读取标准WindowCenter/WindowWidth标签...")
//        val centers = attrs.getDoubles(Tag.WindowCenter)
//        val widths = attrs.getDoubles(Tag.WindowWidth)
//        val descriptions = attrs.getStrings(Tag.WindowCenterWidthExplanation) ?: emptyArray()
//
//        //LogUtil.w("  WindowCenter(0028,1050)数量: ${centers.size}")
//        //LogUtil.w("  WindowWidth(0028,1051)数量: ${widths.size}")
//        //LogUtil.w("  WindowCenterWidthExplanation(0028,1052)数量: ${descriptions.size}")
//
//        if (centers.isNotEmpty() && widths.isNotEmpty()) {
//            //LogUtil.w("✅ 找到标准WW/WL标签，开始配对...")
//
//            // 配对所有窗宽窗位对（取数量较少的那个）
//            val count = min(centers.size, widths.size)
//            for (i in 0 until count) {
//                val desc = if (i < descriptions.size) descriptions[i] else null
//                val window = DicomWindowSettings.DicomWindow(
//                    center = centers[i],
//                    width = widths[i],
//                    description = desc
//                )
//                windows.add(window)
//                //LogUtil.w("  预设${i + 1}: WL=${window.center}, WW=${window.width}, 描述=${window.description ?: "无"}")
//            }
//        } else {
//            //LogUtil.w("❌ 未找到标准WindowCenter/WindowWidth标签")
//        }
//
//        // 第二步：如果没有WW/WL标签，尝试读取VOI LUT Sequence
//        if (windows.isEmpty()) {
//            //LogUtil.w("\n🔍 尝试读取VOI LUT Sequence(0028,3010)...")
//            val voiLutSeq = attrs.getSequence(Tag.VOILUTSequence)
//
//            if (voiLutSeq != null && voiLutSeq.isNotEmpty()) {
//                //LogUtil.w("✅ 找到VOI LUT Sequence，数量: ${voiLutSeq.size}")
//
//                voiLutSeq.forEachIndexed { index, lutItem ->
//                    val lutDescriptor = lutItem.getInts(Tag.LUTDescriptor)
//                    if (lutDescriptor != null && lutDescriptor.size >= 3) {
//                        // LUTDescriptor格式：[条目数, 第一个输入值, 位深度]
//                        val numEntries = lutDescriptor[0]
//                        val firstInput = lutDescriptor[1]
//                        val bitDepth = lutDescriptor[2]
//
//                        //LogUtil.w("  LUT ${index + 1}:")
//                        //LogUtil.w("    LUTDescriptor: 条目数=$numEntries, 第一个输入值=$firstInput, 位深度=$bitDepth")
//
//                        val width = numEntries.toDouble()
//                        val center = firstInput + width / 2.0
//                        val desc = lutItem.getString(Tag.LUTExplanation)
//
//                        val window = DicomWindowSettings.DicomWindow(
//                            center = center,
//                            width = width,
//                            description = desc
//                        )
//                        windows.add(window)
//                        //LogUtil.w("    转换为WW/WL: WL=${window.center}, WW=${window.width}, 描述=${window.description ?: "无"}")
//                    } else {
//                        //LogUtil.w("  ❌ LUT ${index + 1}格式无效或不完整")
//                    }
//                }
//            } else {
//                //LogUtil.w("❌ 未找到VOI LUT Sequence")
//            }
//        }
//
//        return DicomWindowSettings(
//            smallestPixelValue = smallestPixelValue,
//            largestPixelValue = largestPixelValue,
//            windows = windows.toList(),
//            autoCalculatedWindow = autoWindow
//        )
//    }
//}
//
///**
// * 根据DICOM窗宽窗位信息生成适合的SeekBar配置
// * 针对牙科影像优化
// */
//fun DicomWindowSettings.createSeekBarConfigs(): WindowSeekBarConfigs {
//    // 窗宽(WW)范围设计：
//    // - 最小值：100（太小会导致图像变成黑白两色，无诊断价值）
//    // - 最大值：整个像素范围（允许用户看到所有内容）
//    val wwMin = 100.0
//    val wwMax = (largestPixelValue - smallestPixelValue).toDouble()
//    val wwDefault = firstAvailableWindow.width
//
//    // 窗位(WL)范围设计：
//    // - 最小值：像素最小值
//    // - 最大值：像素最大值
//    // 理论上窗位可以在整个像素范围内移动
//    val wlMin = smallestPixelValue.toDouble()
//    val wlMax = largestPixelValue.toDouble()
//    val wlDefault = firstAvailableWindow.center
//
//    return WindowSeekBarConfigs(
//        windowWidthConfig = SeekBarConfig(
//            minValue = wwMin,
//            maxValue = wwMax,
//            defaultValue = wwDefault
//        ),
//        windowCenterConfig = SeekBarConfig(
//            minValue = wlMin,
//            maxValue = wlMax,
//            defaultValue = wlDefault
//        )
//    )
//}
//
//fun dicomFile2Bitmap(file: File): Bitmap {
//    val dr = DicomImageReader()
//    dr.open(file)
//    val ds = dr.attributes
//    val wc = ds.getString(Tag.WindowCenter)
//    val ww = ds.getString(Tag.WindowWidth)
//    LogUtil.d("wc=$wc,ww=$ww")
////    val raster: Raster = dr.applyWindowCenter(0, Integer.parseInt(ww), Integer.parseInt(wc))
//
//    val param = DicomImageReadParam()
//    param.windowWidth = Integer.parseInt(ww).toFloat()
//    param.windowCenter = Integer.parseInt(wc).toFloat()
//    val raster: Raster = dr.applyLUTs(dr.readRaster(0), 0, param, 8)
//    LogUtil.d("raster.getWidth()=" + raster.getWidth() + ",raster.getHeight()=" + raster.getHeight())
//
//    return RasterUtil.rasterToBitmap(raster)
//}
//
//fun dicomFile2Bitmap(file: File, windowWidth: Double, windowCenter: Double): Bitmap {
//    val dr = DicomImageReader()
//    dr.open(file)
//    val param = DicomImageReadParam()
//    param.windowWidth = windowWidth.toFloat()
//    param.windowCenter = windowCenter.toFloat()
//    val raster: Raster = dr.applyLUTs(dr.readRaster(0), 0, param, 8)
//    LogUtil.d("raster.getWidth()=" + raster.getWidth() + ",raster.getHeight()=" + raster.getHeight())
//
//    return RasterUtil.rasterToBitmap(raster)
//}
//
//fun writeDcmFile(
//    record: ScanRecord,
//    rawFile: File,
//    dcmFile: File,
//    imageWidth: Int,
//    imageHeight: Int
//): PixelData? {
////    LogUtil.d("写入dcm文件：检查号${record.examineNo}, raw文件：${rawFile.absolutePath}, dcm文件：${dcmFile.absolutePath}")
//
//    val ds = Attributes(true, 16)
//    ds.setSpecificCharacterSet("ISO_IR 192") // UTF-8 编码
//
//    ds.setString(Tag.InstitutionName, VR.LO, "momo")
//    ds.setString(Tag.Manufacturer, VR.LO, "VRN")
//    ds.setString(Tag.ManufacturerModelName, VR.LO, "EQ800")
//
//    ds.setString(Tag.PatientID, VR.LO, record.examineNo.toString())
//    ds.setString(Tag.PatientName, VR.PN, record.patientName)
//    ds.setString(Tag.PatientAge, VR.AS, record.patientAge)
//    val genderCode = when (record.patientSex) {
//        "男" -> "M"  // DICOM标准值：M（Male）
//        "女" -> "F"  // DICOM标准值：F（Female）
//        else -> "O"  // DICOM标准值：O（Other）
//    }
//    ds.setString(Tag.PatientSex, VR.CS, genderCode)
//
//    ds.setString(Tag.StudyID, VR.LO, record.examineNo.toString())
//    val now = Date()
//    val dicomDate = SimpleDateFormat("yyyyMMdd", Locale.US).format(now)
//    val dicomTime = SimpleDateFormat("HHmmss", Locale.US).format(now)
//    ds.setString(Tag.StudyDate, VR.DA, dicomDate)
//    ds.setString(Tag.StudyTime, VR.TM, dicomTime)
//    ds.setString(Tag.Modality, VR.CS, "CR")
//    ds.setString(Tag.BodyPartExamined, VR.CS, record.toothPosition.toString())
//    ds.setString(Tag.SeriesNumber, VR.IS, "1")
//    ds.setString(Tag.InstanceNumber, VR.IS, "1")
//    ds.setString(Tag.ImageType, VR.CS, "ORIGINAL\\PRIMARY")
//
//    val pixelData = ProcessPixelData.process(rawFile.readBytes(), imageWidth, imageHeight)
//    ds.setInt(Tag.Rows, VR.US, pixelData.rows)
//    ds.setInt(Tag.Columns, VR.US, pixelData.columns)
//    ds.setInt(Tag.SamplesPerPixel, VR.US, 1)
//    ds.setString(Tag.PhotometricInterpretation, VR.CS, "MONOCHROME1")
//    ds.setInt(Tag.BitsAllocated, VR.US, 16)
//    ds.setInt(Tag.BitsStored, VR.US, 16)
//    ds.setInt(Tag.HighBit, VR.US, 15)
//    ds.setInt(Tag.PixelRepresentation, VR.US, 0)
//    ds.setDouble(Tag.PixelSpacing, VR.DS, 0.03369563, 0.03346939)
//    ds.setDouble(Tag.ImagerPixelSpacing, VR.DS, 0.03369563, 0.03346939)
//    ds.setValue(Tag.PixelData, VR.OW, pixelData.data)
//    ds.setString(Tag.WindowWidth, VR.DS, "${pixelData.win_width}")
//    ds.setString(Tag.WindowCenter, VR.DS, "${pixelData.win_center}")
//
//    ds.setDouble(Tag.ExposureIndex, VR.DS, pixelData.exposure_leve.toDouble())
//    ds.setDouble(Tag.TargetExposureIndex, VR.DS, 28000.0)
//    ds.setDouble(Tag.DeviationIndex, VR.DS, 1000.0)
//    ds.setInt(Tag.LargestImagePixelValue, VR.US, pixelData.largestImagePixelValue)
//
//    ds.setString(Tag.SoftwareVersions, VR.LO, "DCMTK 3.6.9")
//    ds.setString(Tag.StationName, VR.SH, "VRN-EQ800")
//    ds.setString(Tag.StudyDescription, VR.LO, "Dental X-Ray")
//    ds.setString(Tag.SeriesDescription, VR.LO, "Tooth Region Scan")
//    ds.setString(Tag.PositionReferenceIndicator, VR.LO, "HFS")
//
//    UIDUtils.setRoot("1.2.123.234")
//    val sopClassUID = UID.ComputedRadiographyImageStorage
//    val sopInstanceUID = UIDUtils.createUID()
//    val studyInstanceUID = UIDUtils.createUID()
//    val seriesInstanceUID = UIDUtils.createUID()
//
//    ds.setString(Tag.SOPClassUID, VR.UI, sopClassUID)
//    ds.setString(Tag.SOPInstanceUID, VR.UI, sopInstanceUID)
//    ds.setString(Tag.StudyInstanceUID, VR.UI, studyInstanceUID)
//    ds.setString(Tag.SeriesInstanceUID, VR.UI, seriesInstanceUID)
//    val fmi = Attributes.createFileMetaInformation(
//        sopInstanceUID,
//        sopClassUID,
//        UID.ExplicitVRLittleEndian
//    )
//
//    DicomOutputStream(dcmFile).use { dos ->
//        dos.writeDataset(fmi, ds)
//    }
//    return pixelData
//}
