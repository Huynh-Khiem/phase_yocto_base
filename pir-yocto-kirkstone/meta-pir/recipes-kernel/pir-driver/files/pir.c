// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/property.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/wait.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>
#include <linux/mutex.h>

#define DRIVER_NAME "pir_driver"
#define DEVICE_NAME "m_device"
#define CLASS_NAME  "m_class"
#define DEFAULT_DEBOUNCE_MS 150

struct pir_dev {
	dev_t dev_num;
	struct class *m_class;
	struct cdev m_cdev;
	struct device *dev;

	struct gpio_desc *pir_gpio;
	int irq;

	struct delayed_work debounce_work;
	u32 debounce_ms;

	wait_queue_head_t wq;
	struct mutex lock;
	int event_ready;
	int last_state;
};

static void pir_debounce_work_fn(struct work_struct *work)
{
	struct pir_dev *pdata = container_of(to_delayed_work(work),
					     struct pir_dev, debounce_work);
	int state;

	state = gpiod_get_value_cansleep(pdata->pir_gpio);
	if (state < 0) {
		dev_err(pdata->dev, "Failed to read PIR GPIO: %d\n", state);
		return;
	}

	mutex_lock(&pdata->lock);
	if (state != pdata->last_state) {
		pdata->last_state = state;
		pdata->event_ready = 1;
		wake_up_interruptible(&pdata->wq);
	}
	mutex_unlock(&pdata->lock);
}

static irqreturn_t pir_irq_handler(int irq, void *dev_id)
{
	struct pir_dev *pdata = dev_id;

	mod_delayed_work(system_wq, &pdata->debounce_work,
			 msecs_to_jiffies(pdata->debounce_ms));

	return IRQ_HANDLED;
}

static int m_open(struct inode *inode, struct file *file)
{
	struct pir_dev *pdata = container_of(inode->i_cdev,
					     struct pir_dev, m_cdev);
	file->private_data = pdata;
	pr_info("pir_driver: open called\n");
	return 0;
}

static int m_release(struct inode *inode, struct file *file)
{
	pr_info("pir_driver: close called\n");
	return 0;
}

static ssize_t m_read(struct file *filp, char __user *user_buf,
		      size_t size, loff_t *offset)
{
	struct pir_dev *pdata = filp->private_data;
	const char *msg;
	size_t len;
	int state;

	if (wait_event_interruptible(pdata->wq, pdata->event_ready != 0))
		return -ERESTARTSYS;

	mutex_lock(&pdata->lock);
	pdata->event_ready = 0;
	state = pdata->last_state;
	mutex_unlock(&pdata->lock);

	msg = state ? "phat hien chuyen dong\n" : "khong phat hien chuyen dong\n";
	len = strlen(msg);

	if (size < len)
		return -EINVAL;

	if (copy_to_user(user_buf, msg, len))
		return -EFAULT;

	return len;
}

static const struct file_operations fops = {
	.owner   = THIS_MODULE,
	.open    = m_open,
	.release = m_release,
	.read    = m_read,
};

static int pir_probe(struct platform_device *pdev)
{
	struct pir_dev *pdata;
	int ret;

	pdata = devm_kzalloc(&pdev->dev, sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		return -ENOMEM;

	pdata->dev = &pdev->dev;
	platform_set_drvdata(pdev, pdata);
	init_waitqueue_head(&pdata->wq);
	mutex_init(&pdata->lock);
	INIT_DELAYED_WORK(&pdata->debounce_work, pir_debounce_work_fn);

	if (device_property_read_u32(&pdev->dev, "debounce-ms", &pdata->debounce_ms))
		pdata->debounce_ms = DEFAULT_DEBOUNCE_MS;

	pdata->pir_gpio = devm_gpiod_get(&pdev->dev, NULL, GPIOD_IN);
	if (IS_ERR(pdata->pir_gpio)) {
		ret = PTR_ERR(pdata->pir_gpio);
		dev_err(&pdev->dev, "Failed to get PIR GPIO: %d\n", ret);
		return ret;
	}

	pdata->last_state = gpiod_get_value_cansleep(pdata->pir_gpio);
	if (pdata->last_state < 0)
		pdata->last_state = 0;

	pdata->irq = gpiod_to_irq(pdata->pir_gpio);
	if (pdata->irq < 0) {
		dev_err(&pdev->dev, "Failed to map GPIO to IRQ: %d\n", pdata->irq);
		return pdata->irq;
	}

	ret = alloc_chrdev_region(&pdata->dev_num, 0, 1, "m-cdev");
	if (ret < 0) {
		dev_err(&pdev->dev, "Failed to alloc chrdev region\n");
		return ret;
	}

	cdev_init(&pdata->m_cdev, &fops);
	pdata->m_cdev.owner = THIS_MODULE;
	ret = cdev_add(&pdata->m_cdev, pdata->dev_num, 1);
	if (ret < 0) {
		dev_err(&pdev->dev, "Failed to add cdev\n");
		goto err_unreg_chrdev;
	}

	/* Linux 5.15 (Yocto Kirkstone) uses 2-argument class_create */
	pdata->m_class = class_create(THIS_MODULE, CLASS_NAME);
	if (IS_ERR(pdata->m_class)) {
		ret = PTR_ERR(pdata->m_class);
		dev_err(&pdev->dev, "Failed to create class\n");
		goto err_cdev_del;
	}

	if (IS_ERR(device_create(pdata->m_class, &pdev->dev,
				 pdata->dev_num, NULL, DEVICE_NAME))) {
		ret = -ENOMEM;
		dev_err(&pdev->dev, "Failed to create device\n");
		goto err_class_destroy;
	}

	ret = request_irq(pdata->irq, pir_irq_handler,
			  IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
			  "pir_gpio17", pdata);
	if (ret < 0) {
		dev_err(&pdev->dev, "Failed to request IRQ %d: %d\n",
			pdata->irq, ret);
		goto err_device_destroy;
	}

	dev_info(&pdev->dev, "PIR driver probed (IRQ=%d, debounce=%ums)\n",
		 pdata->irq, pdata->debounce_ms);
	return 0;

err_device_destroy:
	device_destroy(pdata->m_class, pdata->dev_num);
err_class_destroy:
	class_destroy(pdata->m_class);
err_cdev_del:
	cdev_del(&pdata->m_cdev);
err_unreg_chrdev:
	unregister_chrdev_region(pdata->dev_num, 1);
	return ret;
}

static int pir_remove(struct platform_device *pdev)
{
	struct pir_dev *pdata = platform_get_drvdata(pdev);

	free_irq(pdata->irq, pdata);
	cancel_delayed_work_sync(&pdata->debounce_work);
	device_destroy(pdata->m_class, pdata->dev_num);
	class_destroy(pdata->m_class);
	cdev_del(&pdata->m_cdev);
	unregister_chrdev_region(pdata->dev_num, 1);

	dev_info(&pdev->dev, "PIR driver removed\n");
	return 0;
}

static const struct of_device_id pir_of_match[] = {
	{ .compatible = "khiem,pir-sensor" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, pir_of_match);

static struct platform_driver pir_platform_driver = {
	.probe  = pir_probe,
	.remove = pir_remove,
	.driver = {
		.name           = DRIVER_NAME,
		.of_match_table = pir_of_match,
	},
};

module_platform_driver(pir_platform_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("khiem");
MODULE_DESCRIPTION("PIR Sensor Platform Driver with 150ms Debounce");
