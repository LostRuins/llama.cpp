#include <stdio.h>
#include <string.h>
#include <time.h>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include <inttypes.h>
#include <cinttypes>
#include <algorithm>

#include "model_adapter.h"

#include "stable-diffusion.cpp"
#include "util.cpp"
#include "upscaler.cpp"
#include "model.cpp"
#include "zip.c"

#include "otherarch/utils.h"

// #include "preprocessing.hpp"
#include "stable-diffusion.h"

//#define STB_IMAGE_IMPLEMENTATION //already defined in llava
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include "stb_image_write.h"

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image_resize2.h"

const char* rng_type_to_str[] = {
    "std_default",
    "cuda",
};

// Names of the sampler method, same order as enum sample_method in stable-diffusion.h
const char* sample_method_str[] = {
    "euler_a",
    "euler",
    "heun",
    "dpm2",
    "dpm++2s_a",
    "dpm++2m",
    "dpm++2mv2",
    "lcm",
};

// Names of the sigma schedule overrides, same order as sample_schedule in stable-diffusion.h
const char* schedule_str[] = {
    "default",
    "discrete",
    "karras",
};

const char* modes_str[] = {
    "txt2img",
    "img2img",
    "img2vid",
    "convert",
};

enum SDMode {
    TXT2IMG,
    IMG2IMG,
    IMG2VID,
    CONVERT,
    MODE_COUNT
};

struct SDParams {
    int n_threads = -1;
    SDMode mode   = TXT2IMG;

    std::string model_path;
    std::string vae_path;
    std::string taesd_path;
    std::string esrgan_path;
    std::string controlnet_path;
    std::string embeddings_path;
    sd_type_t wtype = SD_TYPE_COUNT;
    std::string lora_model_dir;
    std::string output_path = "output.png";
    std::string input_path;
    std::string control_image_path;

    std::string prompt;
    std::string negative_prompt;
    float min_cfg   = 1.0f;
    float cfg_scale = 7.0f;
    int clip_skip   = -1;  // <= 0 represents unspecified
    int width       = 512;
    int height      = 512;
    int batch_count = 1;

    int video_frames         = 6;
    int motion_bucket_id     = 127;
    int fps                  = 6;
    float augmentation_level = 0.f;

    sample_method_t sample_method = EULER_A;
    schedule_t schedule           = DEFAULT;
    int sample_steps              = 20;
    float strength                = 0.75f;
    float control_strength        = 0.9f;
    rng_type_t rng_type           = CUDA_RNG;
    int64_t seed                  = 42;
    bool verbose                  = false;
    bool vae_tiling               = false;
    bool control_net_cpu          = false;
    bool canny_preprocess         = false;
    int upscale_repeats           = 1;
};

//shared
int total_img_gens = 0;

//global static vars for SD
static SDParams * sd_params = nullptr;
static sd_ctx_t * sd_ctx = nullptr;
static int sddebugmode = 0;
static std::string recent_data = "";

std::string base64_encode(const unsigned char* data, unsigned int data_length) {
    const std::string base64_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    encoded.reserve(((data_length + 2) / 3) * 4);
    for (unsigned int i = 0; i < data_length; i += 3) {
        unsigned int triple = (data[i] << 16) + (i + 1 < data_length ? data[i + 1] << 8 : 0) + (i + 2 < data_length ? data[i + 2] : 0);
        encoded.push_back(base64_chars[(triple >> 18) & 0x3F]);
        encoded.push_back(base64_chars[(triple >> 12) & 0x3F]);
        if (i + 1 < data_length) {
            encoded.push_back(base64_chars[(triple >> 6) & 0x3F]);
        } else {
            encoded.push_back('=');
        }
        if (i + 2 < data_length) {
            encoded.push_back(base64_chars[triple & 0x3F]);
        } else {
            encoded.push_back('=');
        }
    }
    return encoded;
}

static std::string sdplatformenv, sddeviceenv, sdvulkandeviceenv;
bool sdtype_load_model(const sd_load_model_inputs inputs) {

    executable_path = inputs.executable_path;
    std::string taesdpath = "";
    std::string lorafilename = inputs.lora_filename;
    std::string vaefilename = inputs.vae_filename;
    printf("\nImageGen Init - Load Model: %s\n",inputs.model_filename);
    if(lorafilename!="")
    {
        printf("With LoRA: %s at %f power\n",lorafilename.c_str(),inputs.lora_multiplier);
    }
    if(inputs.taesd)
    {
        taesdpath = executable_path + "taesd.embd";
        printf("With TAE SD VAE: %s\n",taesdpath.c_str());
    }
    else if(vaefilename!="")
    {
        printf("With Custom VAE: %s\n",vaefilename.c_str());
    }

    //duplicated from expose.cpp
    int cl_parseinfo = inputs.clblast_info; //first digit is whether configured, second is platform, third is devices
    std::string usingclblast = "GGML_OPENCL_CONFIGURED="+std::to_string(cl_parseinfo>0?1:0);
    putenv((char*)usingclblast.c_str());
    cl_parseinfo = cl_parseinfo%100; //keep last 2 digits
    int platform = cl_parseinfo/10;
    int devices = cl_parseinfo%10;
    sdplatformenv = "GGML_OPENCL_PLATFORM="+std::to_string(platform);
    sddeviceenv = "GGML_OPENCL_DEVICE="+std::to_string(devices);
    putenv((char*)sdplatformenv.c_str());
    putenv((char*)sddeviceenv.c_str());
    std::string vulkan_info_raw = inputs.vulkan_info;
    std::string vulkan_info_str = "";
    for (size_t i = 0; i < vulkan_info_raw.length(); ++i) {
        vulkan_info_str += vulkan_info_raw[i];
        if (i < vulkan_info_raw.length() - 1) {
            vulkan_info_str += ",";
        }
    }
    if(vulkan_info_str=="")
    {
        vulkan_info_str = "0";
    }
    sdvulkandeviceenv = "GGML_VK_VISIBLE_DEVICES="+vulkan_info_str;
    putenv((char*)sdvulkandeviceenv.c_str());

    sd_params = new SDParams();
    sd_params->model_path = inputs.model_filename;
    sd_params->wtype = (inputs.quant==0?SD_TYPE_F16:SD_TYPE_Q4_0);
    sd_params->n_threads = inputs.threads; //if -1 use physical cores
    sd_params->input_path = ""; //unused
    sd_params->batch_count = 1;
    sd_params->vae_path = vaefilename;
    sd_params->taesd_path = taesdpath;

    sddebugmode = inputs.debugmode;

    set_sd_log_level(sddebugmode);

    bool vae_decode_only = false;
    bool free_param = false;
    if(inputs.debugmode==1)
    {
        printf("\nMODEL:%s\nVAE:%s\nTAESD:%s\nCNET:%s\nLORA:%s\nEMBD:%s\nVAE_DEC:%d\nVAE_TILE:%d\nFREE_PARAM:%d\nTHREADS:%d\nWTYPE:%d\nRNGTYPE:%d\nSCHED:%d\nCNETCPU:%d\n\n",
        sd_params->model_path.c_str(),
        sd_params->vae_path.c_str(),
        sd_params->taesd_path.c_str(),
        sd_params->controlnet_path.c_str(),
        sd_params->lora_model_dir.c_str(),
        sd_params->embeddings_path.c_str(),
        vae_decode_only,
        sd_params->vae_tiling,
        free_param,
        sd_params->n_threads,
        sd_params->wtype,
        sd_params->rng_type,
        sd_params->schedule,
        sd_params->control_net_cpu);
    }

    sd_ctx = new_sd_ctx(sd_params->model_path.c_str(),
                        sd_params->vae_path.c_str(),
                        sd_params->taesd_path.c_str(),
                        sd_params->controlnet_path.c_str(),
                        sd_params->lora_model_dir.c_str(),
                        sd_params->embeddings_path.c_str(),
                        vae_decode_only,
                        sd_params->vae_tiling,
                        free_param,
                        sd_params->n_threads,
                        sd_params->wtype,
                        sd_params->rng_type,
                        sd_params->schedule,
                        sd_params->control_net_cpu);

    if (sd_ctx == NULL) {
        printf("\nError: KCPP SD Failed to create context!\n");
        return false;
    }

    if(lorafilename!="" && inputs.lora_multiplier>0)
    {
        printf("\nApply LoRA...\n");
       // sd_ctx->sd->set_pending_lora(lorafilename,inputs.lora_multiplier);
        sd_ctx->sd->apply_lora_from_file(lorafilename,inputs.lora_multiplier);
    }

    return true;

}

std::string clean_input_prompt(const std::string& input) {
    std::string result;
    result.reserve(input.size());
    for (char ch : input) {
        // Check if the character is an ASCII or extended ASCII character
        if (static_cast<unsigned char>(ch) <= 0x7F || (ch >= 0xC2 && ch <= 0xF4)) {
            result.push_back(ch);
        }
    }
    //limit to max 800 chars
    result = result.substr(0, 800);
    return result;
}

sd_generation_outputs sdtype_generate(const sd_generation_inputs inputs)
{
    sd_generation_outputs output;

    if(sd_ctx == nullptr || sd_params == nullptr)
    {
        printf("\nWarning: KCPP image generation not initialized!\n");
        output.data = "";
        output.status = 0;
        return output;
    }
    uint8_t * input_image_buffer = NULL;
    sd_image_t * results;
    sd_image_t* control_image = NULL;

    bool is_quiet = inputs.quiet;
    set_sd_quiet(is_quiet);

    //sanitize prompts, remove quotes and limit lengths
    std::string cleanprompt = clean_input_prompt(inputs.prompt);
    std::string cleannegprompt = clean_input_prompt(inputs.negative_prompt);
    std::string img2img_data = std::string(inputs.init_images);

    sd_params->prompt = cleanprompt;
    sd_params->negative_prompt = cleannegprompt;
    sd_params->cfg_scale = inputs.cfg_scale;
    sd_params->sample_steps = inputs.sample_steps;
    sd_params->seed = inputs.seed;
    sd_params->width = inputs.width;
    sd_params->height = inputs.height;
    sd_params->strength = inputs.denoising_strength;
    sd_params->clip_skip = inputs.clip_skip;
    sd_params->mode = (img2img_data==""?SDMode::TXT2IMG:SDMode::IMG2IMG);

    //for img2img
    sd_image_t input_image = {0,0,0,nullptr};
    std::vector<uint8_t> image_buffer;
    int nx, ny, nc;
    int img2imgW = inputs.width; //for img2img input
    int img2imgH = inputs.height;
    int img2imgC = 3; // Assuming RGB image
    std::vector<uint8_t> resized_image_buf(img2imgW * img2imgH * img2imgC);

    if(!is_quiet)
    {
        printf("\nGenerating Image (%d steps)\n",inputs.sample_steps);
    }else{
        printf("\nGenerating (%d st.)\n",inputs.sample_steps);
    }

    fflush(stdout);
    std::string sampler = inputs.sample_method;

    if(sampler=="euler a"||sampler=="k_euler_a"||sampler=="euler_a") //all lowercase
    {
        sd_params->sample_method = sample_method_t::EULER_A;
    }
    else if(sampler=="euler"||sampler=="k_euler")
    {
        sd_params->sample_method = sample_method_t::EULER;
    }
    else if(sampler=="heun"||sampler=="k_heun")
    {
        sd_params->sample_method = sample_method_t::HEUN;
    }
    else if(sampler=="dpm2"||sampler=="k_dpm_2")
    {
        sd_params->sample_method = sample_method_t::DPM2;
    }
    else if(sampler=="lcm"||sampler=="k_lcm")
    {
        sd_params->sample_method = sample_method_t::LCM;
    }
    else if(sampler=="dpm++ 2m karras" || sampler=="dpm++ 2m" || sampler=="k_dpmpp_2m")
    {
        sd_params->sample_method = sample_method_t::DPMPP2M;
    }
    else
    {
        sd_params->sample_method = sample_method_t::EULER_A;
    }

    if (sd_params->mode == TXT2IMG) {

        if(!is_quiet && sddebugmode==1)
        {
            printf("\nTXT2IMG PROMPT:%s\nNPROMPT:%s\nCLPSKP:%d\nCFGSCLE:%f\nW:%d\nH:%d\nSM:%d\nSTEP:%d\nSEED:%d\nBATCH:%d\nCIMG:%p\nCSTR:%f\n\n",
            sd_params->prompt.c_str(),
            sd_params->negative_prompt.c_str(),
            sd_params->clip_skip,
            sd_params->cfg_scale,
            sd_params->width,
            sd_params->height,
            sd_params->sample_method,
            sd_params->sample_steps,
            (int)sd_params->seed,
            sd_params->batch_count,
            control_image,
            sd_params->control_strength);
        }
        results = txt2img(sd_ctx,
                          sd_params->prompt.c_str(),
                          sd_params->negative_prompt.c_str(),
                          sd_params->clip_skip,
                          sd_params->cfg_scale,
                          sd_params->width,
                          sd_params->height,
                          sd_params->sample_method,
                          sd_params->sample_steps,
                          sd_params->seed,
                          sd_params->batch_count,
                          control_image,
                          sd_params->control_strength);
    } else {

        if (sd_params->width <= 0 || sd_params->width % 64 != 0 || sd_params->height <= 0 || sd_params->height % 64 != 0) {
            printf("\nKCPP SD: bad request image dimensions!\n");
            output.data = "";
            output.status = 0;
            return output;
        }

        image_buffer = kcpp_base64_decode(img2img_data);

        if(input_image_buffer!=nullptr) //just in time free old buffer
        {
             stbi_image_free(input_image_buffer);
             input_image_buffer = nullptr;
        }

        input_image_buffer = stbi_load_from_memory(image_buffer.data(), image_buffer.size(), &nx, &ny, &nc, 3);

        if (nx < 64 || ny < 64 || nx > 1024 || ny > 1024 || nc!= 3) {
            printf("\nKCPP SD: bad input image dimensions %d x %d!\n",nx,ny);
            output.data = "";
            output.status = 0;
            return output;
        }
        if (!input_image_buffer) {
            printf("\nKCPP SD: load image from memory failed!\n");
            output.data = "";
            output.status = 0;
            return output;
        }

        // Resize the image
        unsigned char * resok = stbir_resize_uint8_linear(input_image_buffer, nx, ny, 0, resized_image_buf.data(), img2imgW, img2imgH, 0, (stbir_pixel_layout)img2imgC);
        if (!resok) {
            printf("\nKCPP SD: resize image failed!\n");
            output.data = "";
            output.status = 0;
            return output;
        }

        input_image.width = img2imgW;
        input_image.height = img2imgH;
        input_image.channel = img2imgC;
        input_image.data = resized_image_buf.data();

        if(!is_quiet && sddebugmode==1)
        {
            printf("\nIMG2IMG PROMPT:%s\nNPROMPT:%s\nCLPSKP:%d\nCFGSCLE:%f\nW:%d\nH:%d\nSM:%d\nSTEP:%d\nSEED:%d\nBATCH:%d\nCIMG:%p\nSTR:%f\n\n",
            sd_params->prompt.c_str(),
            sd_params->negative_prompt.c_str(),
            sd_params->clip_skip,
            sd_params->cfg_scale,
            sd_params->width,
            sd_params->height,
            sd_params->sample_method,
            sd_params->sample_steps,
            (int)sd_params->seed,
            sd_params->batch_count,
            control_image,
            sd_params->strength);
        }

        results = img2img(sd_ctx,
                            input_image,
                            sd_params->prompt.c_str(),
                            sd_params->negative_prompt.c_str(),
                            sd_params->clip_skip,
                            sd_params->cfg_scale,
                            sd_params->width,
                            sd_params->height,
                            sd_params->sample_method,
                            sd_params->sample_steps,
                            sd_params->strength,
                            sd_params->seed,
                            sd_params->batch_count);
    }

    if (results == NULL) {
        printf("\nKCPP SD generate failed!\n");
        output.data = "";
        output.status = 0;
        return output;
    }


    for (int i = 0; i < sd_params->batch_count; i++) {
        if (results[i].data == NULL) {
            continue;
        }

        int out_data_len;
        unsigned char * png = stbi_write_png_to_mem(results[i].data, 0, results[i].width, results[i].height, results[i].channel, &out_data_len, "");
        if (png != NULL)
        {
            recent_data = base64_encode(png,out_data_len);
            free(png);
        }

        free(results[i].data);
        results[i].data = NULL;
    }

    free(results);
    output.data = recent_data.c_str();
    output.status = 1;
    total_img_gens += 1;
    return output;
}