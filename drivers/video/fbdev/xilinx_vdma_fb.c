/*
* Xilinix vdma fb driver
*
*/
#include <linux/device.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/fb.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/dma/xilinx_dma.h>
#include <linux/clk.h>
#include <media/xilinx-vtc.h>

typedef struct {
	char label[64];  /* Label describing the resolution */
	u32 width;		 /* Width(horizon) of the active video frame */
	u32 height; 	 /* Height(vertical) of the active video frame */
	u32 hps; 		 /* Start time of Horizontal sync pulse, in pixel clocks (active width + H. front porch) */
	u32 hpe; 		 /* End time of Horizontal sync pulse, in pixel clocks (active width + H. front porch + H. sync width) */
	u32 vps;         /* Start time of Vertical sync pulse, in lines (active height + V. front porch) */
	u32 vpe;         /* End time of Vertical sync pulse, in lines (active height + V. front porch + V. sync width) */
	u32 hmax; 		 /* Total number of pixel clocks per line (active width + H. front porch + H. sync width + H. back porch) */
	u32 vmax; 		 /* Total number of lines per frame (active height + V. front porch + V. sync width + V. back porch) */
	double freq; 	 /* Pixel Clock frequency */
} s_vtcParameter;

static const s_vtcParameter vtc_parameter[] =
{
	{ "640x480@60Hz", 	640, 	480, 	656, 	752,	489, 	491,  	800,  	525, 	25.0 },
	{ "800x600@60Hz", 	800, 	600, 	840, 	968, 	600, 	604,  	1056, 	628, 	40.0 },
	{ "1280x720@60Hz", 	1280, 	720, 	1390, 	1430, 	724, 	729,  	1650, 	750, 	74.25 },
	{ "1280x1024@60Hz", 1280, 	1024, 	1328, 	1440, 	1024, 	1027, 	1688, 	1065, 	108.0 },
	{ "1920x1080@60Hz", 1920, 	1080, 	2008, 	2052, 	1083, 	1088, 	2200, 	1125, 	148.5 },
};

#define DYNCLK_RATE 148500000

#define BITS_PER_PIXEL 32	/*color deepth of the screen*/
#define RED_SHIFT	16
#define GREEN_SHIFT	8
#define BLUE_SHIFT	0

#define PALETTE_ENTRIES_NO	256	/* passed to fb_alloc_cmap() */


/**
 * some parameters of screen output
 */

struct xilinx_vdma_fb_conf
{
	uint32_t resolution_height;
	uint32_t resolution_width;
};

/**
 * default parameters of screen
 */
static struct xilinx_vdma_fb_conf xilinx_vdma_fb_default_conf =
{
	.resolution_height = 1080,
	.resolution_width = 1920
};

struct xilinx_vdma_fb_drvdata
{
	struct fb_info info;
	uint32_t pseudo_palette[16];
	struct dma_chan *mm2s_dma_chan;
	struct xilinx_vdma_config vdma_config; 
	void *fb_virtual;
	dma_addr_t fb_phy;
	void __iomem *regs;
	struct xilinx_vdma_fb_conf fb_conf;
	struct clk *dyn_clk; 
	struct xvtc_config vtc_config;
	struct xvtc_device *vtc_device;
};

/**
 * copy from xilinxfb, need modify later
 * 
 * @author xczhang (5/9/18)
 */
static const struct fb_fix_screeninfo xilinx_vdma_fb_fix = {
	.id =		"Xilinx VDMA",
	.type =		FB_TYPE_PACKED_PIXELS,
	.visual =	FB_VISUAL_TRUECOLOR,
	.accel =	FB_ACCEL_NONE
}; 

/**
 * copy from xilinxfb, need modify later
 * 
 * @author xczhang (5/9/18)
 */
static const struct fb_var_screeninfo xilinx_fb_var = {
	.bits_per_pixel =	BITS_PER_PIXEL,

	.red =		{ RED_SHIFT, 8, 0 },
	.green =	{ GREEN_SHIFT, 8, 0 },
	.blue =		{ BLUE_SHIFT, 8, 0 },
	.transp =	{ 0, 0, 0 },

	.activate =	FB_ACTIVATE_NOW
}; 

static struct dma_interleaved_template dma_tmplt; 
static int xilinx_vdma_fb_setcolreg(unsigned int regno, unsigned int red, unsigned int green,
	unsigned int blue, unsigned int transp, struct fb_info *fbi);

/**
 * copy from xilinxfb need update
 * 
 * @author xczhang (5/10/18)
 */
static struct fb_ops xilinx_vdma_fb_ops = {
	.owner			= THIS_MODULE,

	.fb_setcolreg		= xilinx_vdma_fb_setcolreg,
	.fb_fillrect		= cfb_fillrect,
	.fb_copyarea		= cfb_copyarea,
	.fb_imageblit		= cfb_imageblit,
}; 


/**
 * copy from xilinx fb need update
 * 
 * @author xczhang (5/10/18)
 * 
 * @param regno 
 * @param red 
 * @param green 
 * @param blue 
 * @param transp 
 * @param fbi 
 * 
 * @return int 
 */
static int xilinx_vdma_fb_setcolreg(unsigned int regno, unsigned int red, unsigned int green, 
	unsigned int blue, unsigned int transp, struct fb_info *fbi) 
{
	u32 *palette = fbi->pseudo_palette;

	if(regno >= PALETTE_ENTRIES_NO)
		return -EINVAL;

	if(fbi->var.grayscale)
	{
		/* Convert color to grayscale.
		 * grayscale = 0.30*R + 0.59*G + 0.11*B
		 */
		blue = (red * 77 + green * 151 + blue * 28 + 127) >> 8;
		green = blue;
		red = green;
	}

	/* fbi->fix.visual is always FB_VISUAL_TRUECOLOR */

	/* We only handle 8 bits of each color. */
	red >>= 8;
	green >>= 8;
	blue >>= 8;
	palette[regno] = (red << RED_SHIFT) | (green << GREEN_SHIFT) |
		(blue << BLUE_SHIFT);

	return 0;
}

static int framebuffer_init(struct platform_device *pdev)
{
	struct xilinx_vdma_fb_drvdata *drvdata;
	int ret;

	drvdata = platform_get_drvdata(pdev);
	if(!drvdata)
	{
		dev_err(&pdev->dev, "platform_get_drvdata failed!\r\n");
		return -1;
	}

/* Fill struct fb_info */
	drvdata->info.device = &pdev->dev;
	drvdata->info.par = drvdata;
	/*visual address of buffer*/
	drvdata->info.screen_buffer = drvdata->fb_virtual;

	drvdata->info.fbops = &xilinx_vdma_fb_ops;
	drvdata->info.fix = xilinx_vdma_fb_fix;
	drvdata->info.fix.smem_start = drvdata->fb_phy; /*screen mem start address(physical address)*/
	drvdata->info.fix.smem_len = drvdata->fb_conf.resolution_height * drvdata->fb_conf.resolution_width * BITS_PER_PIXEL / 8;
	drvdata->info.fix.line_length = drvdata->fb_conf.resolution_width * BITS_PER_PIXEL / 8;

	drvdata->info.pseudo_palette = drvdata->pseudo_palette;
	drvdata->info.flags = FBINFO_DEFAULT;
	drvdata->info.var = xilinx_fb_var;
	drvdata->info.var.height = drvdata->fb_conf.resolution_height;
	drvdata->info.var.width = drvdata->fb_conf.resolution_width;
	drvdata->info.var.xres = drvdata->fb_conf.resolution_width;
	drvdata->info.var.yres = drvdata->fb_conf.resolution_height;
	drvdata->info.var.xres_virtual = drvdata->fb_conf.resolution_width;
	drvdata->info.var.yres_virtual = drvdata->fb_conf.resolution_height;

	dev_dbg(&pdev->dev, "ready for fb alloc cmap\r\n");
	ret = fb_alloc_cmap(&drvdata->info.cmap, PALETTE_ENTRIES_NO, 0);
	if(ret)
	{
		pr_err("fb alloc cmap failed\r\n");
		return -1;
	}
	dev_dbg(&pdev->dev, "fb alloc cmap success\r\n");
	ret = register_framebuffer(&drvdata->info);
	if(ret)
	{
		pr_err("regist framebuffer failed\r\n");
		return -1;
	}
}

static int vdma_init(struct platform_device *pdev)
{
	struct xilinx_vdma_fb_drvdata *drvdata;
	enum dma_ctrl_flags flags = 0;
	struct dma_async_tx_descriptor *txd = NULL;
	uint32_t buf_len;
	dma_cookie_t tx_cookie = 0;
	int ret;
	drvdata = platform_get_drvdata(pdev);
	if(!drvdata)
	{
		dev_err(&pdev->dev, "platform_get_drvdata failed!\r\n");
		return -1;
	}
	drvdata->mm2s_dma_chan = dma_request_slave_channel(&pdev->dev, "vdma0");
	if(!drvdata->mm2s_dma_chan)
	{
		pr_err("request dma channel failed!\r\n");
		return -1;
	}
	dev_dbg(&pdev->dev, "dma channel:%x\r\n", drvdata->mm2s_dma_chan);

	memset(&drvdata->vdma_config, 0, sizeof(struct xilinx_vdma_config));
	drvdata->vdma_config.coalesc = 0; // Interrupt coalescing threshold
	drvdata->vdma_config.park = 1;
	drvdata->vdma_config.park_frm = 0;
	drvdata->vdma_config.delay = 0;
	ret = xilinx_vdma_channel_set_config(drvdata->mm2s_dma_chan, &drvdata->vdma_config);
	if(ret)
	{
		pr_err("vdma config failed!\r\n");
		return -1;
	}
	dev_dbg(&pdev->dev, "vdma channel set config ok\r\n", drvdata->mm2s_dma_chan);

	drvdata->fb_virtual = dma_alloc_coherent(&pdev->dev, PAGE_ALIGN(drvdata->fb_conf.resolution_height * drvdata->fb_conf.resolution_width * BITS_PER_PIXEL / 8),
		&drvdata->fb_phy, GFP_KERNEL);
	if(!drvdata->fb_virtual)
	{
		pr_err("Could not allocate frame buffer memory\n");
		ret = ENOMEM;
		return -1;
	}
	dev_dbg(&pdev->dev, "dma alloc ok\r\n");

	memset_io((void __iomem *)drvdata->fb_virtual, 0x41,
		PAGE_ALIGN(drvdata->fb_conf.resolution_height * drvdata->fb_conf.resolution_width * BITS_PER_PIXEL / 8));

	dmaengine_terminate_all(drvdata->mm2s_dma_chan);

	dma_tmplt.src_start = drvdata->fb_phy;
	dma_tmplt.dir = DMA_MEM_TO_DEV;
	dma_tmplt.numf = drvdata->fb_conf.resolution_height;
	dma_tmplt.src_sgl = 0;
	dma_tmplt.src_inc = 1;
	dma_tmplt.dst_inc = 0;
	dma_tmplt.dst_sgl = 0;
	dma_tmplt.sgl[0].size = drvdata->fb_conf.resolution_width * BITS_PER_PIXEL / 8;
	dma_tmplt.sgl[0].icg = 0;
	dma_tmplt.frame_size = 1;
	buf_len = drvdata->fb_conf.resolution_height * drvdata->fb_conf.resolution_width * BITS_PER_PIXEL / 8;
	txd = dmaengine_prep_interleaved_dma(drvdata->mm2s_dma_chan, &dma_tmplt, 0);
	//txd = drvdata->mm2s_dma_chan->device->device_prep_interleaved_dma(drvdata->mm2s_dma_chan,
	//	&dma_tmplt, flags);
	if(!txd)
	{
		pr_err("device_prep_interleaved_dma fail!\r\n");
	}
	dev_dbg(&pdev->dev, "device_prep_interleaved_dma ok\r\n");
	tx_cookie = dmaengine_submit(txd);
	//tx_cookie = txd->tx_submit(txd);
	if(!tx_cookie)
	{
		pr_err("tx_submit fail!\r\n");
	}
	dev_dbg(&pdev->dev, "tx_submit ok\r\n");
	dma_async_issue_pending(drvdata->mm2s_dma_chan);
}

static int dynclk_init(struct platform_device *pdev)
{
	struct xilinx_vdma_fb_drvdata *drvdata;
	unsigned long current_clk = 0, set_clk = 0;
	dev_err(&pdev->dev, "entering dynclk_init!\r\n");
	drvdata = platform_get_drvdata(pdev);
	if(!drvdata)
	{
		dev_err(&pdev->dev, "platform_get_drvdata failed!\r\n");
		return -1;
	}
	drvdata->dyn_clk = devm_clk_get(&pdev->dev, "dynclk");
	if(IS_ERR(drvdata->dyn_clk))
	{
		ERR_PTR(drvdata->dyn_clk);
		dev_err(&pdev->dev, "get dyn_clk failed:%x!\r\n", drvdata->dyn_clk);
		return -1;
	}
	if(clk_prepare_enable(drvdata->dyn_clk) != 0)
	{
		dev_err(&pdev->dev, "clk prepare failed!\r\n");
		return -1;
	}
	current_clk = clk_get_rate(drvdata->dyn_clk);
	dev_dbg(&pdev->dev, "current clk rate: %lu\r\n", current_clk);
	set_clk = clk_round_rate(drvdata->dyn_clk, DYNCLK_RATE);
	if(clk_set_rate(drvdata->dyn_clk, set_clk) != 0)
	{
		dev_err(&pdev->dev, "clk_set_rate failed!\r\n");
		return -1;
	}
	return 0;
}

static int xvtc_init(struct platform_device *pdev)
{
	struct xilinx_vdma_fb_drvdata *drvdata;
	drvdata = platform_get_drvdata(pdev);
	if(!drvdata)
	{
		dev_err(&pdev->dev, "platform_get_drvdata failed!\r\n");
		return -1;
	}
	drvdata->vtc_device = xvtc_of_get(pdev->dev.of_node);
	if(IS_ERR(drvdata->vtc_device))
	{
		dev_err(&pdev->dev, "xvtc_of_get failed!\r\n");
		return -1;
	}
	else
	{
		dev_dbg(&pdev->dev, "drvdata->vtc_device addr: 0x%x\r\n", drvdata->vtc_device);
	}

	drvdata->vtc_config.hblank_start = vtc_parameter[4].width;
	drvdata->vtc_config.vblank_start = vtc_parameter[4].height;
	drvdata->vtc_config.hsync_start = vtc_parameter[4].hps;
	drvdata->vtc_config.hsync_end = vtc_parameter[4].hpe;
	drvdata->vtc_config.vsync_start = vtc_parameter[4].vps;
	drvdata->vtc_config.vsync_end = vtc_parameter[4].vpe;
	drvdata->vtc_config.hsize = vtc_parameter[4].hmax;
	drvdata->vtc_config.vsize = vtc_parameter[4].vmax;

	xvtc_generator_start(drvdata->vtc_device, &drvdata->vtc_config);
	return 0;
}


static int xilinx_vdma_fb_probe(struct platform_device *pdev)
{
	int ret;
	struct xilinx_vdma_fb_drvdata *drvdata; 
	struct resource *res;
	dev_dbg(& pdev->dev, "start xilinx vdma fb probe\r\n");

	drvdata = devm_kzalloc(&pdev->dev, sizeof(struct xilinx_vdma_fb_drvdata), GFP_KERNEL);
	if(!drvdata)
		return -ENOMEM;
	platform_set_drvdata(pdev, drvdata);
	/*todo: read parameter from probe*/

	drvdata->fb_conf = xilinx_vdma_fb_default_conf;

	/*init clk*/
	if(dynclk_init(pdev) < 0)
	{
		dev_err(&pdev->dev, "dynclk_init failed\n");

	}
	else
		pr_err("dynclk_init ok!\r\n");

	/*init xvtc*/
	if(xvtc_init(pdev) < 0)
	{
		dev_err(&pdev->dev, "xvtc_init failed\n");
	}
	else
		pr_err("xvtc_init ok!\r\n");

	/*init vdma*/
	if(vdma_init(pdev) < 0)
	{
		dev_err(&pdev->dev, "vdma_init failed\r\n");
	}
	else
		pr_err("vdma_init ok!\r\n");

	/*init framebuffer*/
	if(framebuffer_init(pdev) < 0)
	{
		dev_err(&pdev->dev, "framebuffer_init failed\r\n");
	}
	else
		pr_err("vdma_init ok!\r\n");

	return 0;
error_regfb:
	fb_dealloc_cmap(&drvdata->info.cmap);
error:
	if(drvdata->fb_virtual)
	{
		dma_free_coherent(&pdev->dev, PAGE_ALIGN(drvdata->fb_conf.resolution_height * drvdata->fb_conf.resolution_width * BITS_PER_PIXEL / 8),
			drvdata->fb_virtual, drvdata->fb_phy);
	}
	if(drvdata->mm2s_dma_chan)
	{
		dma_release_channel(drvdata->mm2s_dma_chan);
	}
	return ret;
}

static int xilinx_vdma_fb_remove(struct platform_device *pdev)
{
	struct xilinx_vdma_fb_drvdata* drvdata = platform_get_drvdata(pdev); 
	struct xilinx_vdma_fb_conf fb_conf = xilinx_vdma_fb_default_conf;
	unregister_framebuffer(&drvdata->info);
	fb_dealloc_cmap(&drvdata->info.cmap); 
	dma_free_coherent(&pdev->dev, PAGE_ALIGN(fb_conf.resolution_height * fb_conf.resolution_width * BITS_PER_PIXEL / 8),
		drvdata->fb_virtual, drvdata->fb_phy);
	dma_release_channel(drvdata->mm2s_dma_chan);
	return 0;
}

static const struct of_device_id xilinx_vdma_fb_match[] = {
	{ .compatible = "xlnx,vdma-fb", },
	{ },/*end null member, must add*/
};

static struct platform_driver xilinx_vdma_fb_driver = {
	.driver = {
		.name = "xlnx-vdma-fb",
		.owner = THIS_MODULE,
		.of_match_table = xilinx_vdma_fb_match,
	},
	.probe = xilinx_vdma_fb_probe,
	.remove = xilinx_vdma_fb_remove,
};

module_platform_driver(xilinx_vdma_fb_driver);

MODULE_AUTHOR("xczhang");
MODULE_DESCRIPTION("Xilinx vdma frame buffer driver");
MODULE_LICENSE("GPL");
